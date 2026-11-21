#include "ui/grid/grid_frame_builder.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace capybaradb::ui::grid {

float compute_row_header_width_dip(uint64_t total_rows) noexcept {
    uint64_t n = (total_rows > 0) ? total_rows : 1;
    int digits = 0;
    while (n > 0) {
        n /= 10;
        ++digits;
    }
    return (std::max)(kMinRowHeaderWidthDip, std::round(10.0f + 7.0f * static_cast<float>(digits)));
}

GridFrameBuilder::GridFrameBuilder() {
    rects_.reserve(4096);
    glyphs_.reserve(32768);
    non_ascii_scratch_.reserve(256);
}

void GridFrameBuilder::push_rect(
    float x, float y, float w, float h,
    uint32_t rgba,
    uint32_t style_flags,
    float cx0, float cy0, float cx1, float cy1) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    rects_.push_back(RectInstance{
        .x           = std::floor(x),
        .y           = std::floor(y),
        .w           = std::floor(w),
        .h           = std::floor(h),
        .clip_x0     = cx0,
        .clip_y0     = cy0,
        .clip_x1     = cx1,
        .clip_y1     = cy1,
        .rgba        = rgba,
        .style_flags = style_flags,
    });
}

float GridFrameBuilder::emit_text_fast(
    GlyphAtlas& atlas,
    std::string_view text,
    uint8_t cell_flags,
    float x_phys,
    float baseline_phys,
    float avail_w_phys,
    uint32_t rgba,
    float cx0, float cy0, float cx1, float cy1) {
    if (text.empty() || avail_w_phys <= 1.0f) {
        return 0.0f;
    }

    const GlyphMetric& ell = atlas.ellipsis();
    const float ell_w = ell.valid ? ell.advance_x : 0.0f;
    const float start_x = x_phys;

    // Non-ASCII path -> HarfBuzz shaping cache (with inline whitespace flattening into reusable scratch)
    if ((cell_flags & core::kCellNonAscii) != 0) {
        non_ascii_scratch_.clear();
        const size_t cap = (std::min<size_t>)(text.size(), 256u);
        for (size_t i = 0; i < cap; ++i) {
            const char c = text[i];
            if (c == '\r') {
                if (i + 1 < cap && text[i + 1] == '\n') {
                    ++i;
                }
                non_ascii_scratch_.push_back(' ');
            } else if (c == '\n' || c == '\t') {
                non_ascii_scratch_.push_back(' ');
            } else {
                non_ascii_scratch_.push_back(c);
            }
        }
        // Keep UTF-8 boundary valid if truncated at 256 bytes
        while (!non_ascii_scratch_.empty() &&
               (static_cast<unsigned char>(non_ascii_scratch_.back()) & 0xC0u) == 0x80u) {
            non_ascii_scratch_.pop_back();
        }

        const auto& shaped = atlas.shape_utf8_cached(non_ascii_scratch_);
        float cursor_x = x_phys;
        bool truncated = false;
        size_t total_drawn = 0;

        for (size_t i = 0; i < shaped.size(); ++i) {
            const ShapedGlyph& sg = shaped[i];
            const bool more_after = (i + 1 < shaped.size()) || (text.size() > cap);
            const float limit = more_after ? (avail_w_phys - ell_w) : avail_w_phys;
            if ((cursor_x - start_x) + sg.x_advance > limit && total_drawn > 0) {
                truncated = true;
                break;
            }
            if ((cursor_x - start_x) + sg.x_advance > avail_w_phys) {
                truncated = true;
                break;
            }
            const GlyphMetric& m = atlas.glyph_by_id(sg.glyph_id);
            if (m.valid && m.w > 0.0f && m.h > 0.0f) {
                glyphs_.push_back(GlyphInstance{
                    .x       = std::floor(cursor_x + sg.x_offset + m.bearing_x),
                    .y       = std::floor(baseline_phys - sg.y_offset - m.bearing_y),
                    .w       = m.w,
                    .h       = m.h,
                    .u0      = m.u0,
                    .v0      = m.v0,
                    .u1      = m.u1,
                    .v1      = m.v1,
                    .clip_x0 = cx0,
                    .clip_y0 = cy0,
                    .clip_x1 = cx1,
                    .clip_y1 = cy1,
                    .rgba    = rgba,
                    .sx      = m.sx,
                    .sy      = m.sy,
                });
            }
            cursor_x += sg.x_advance;
            ++total_drawn;
        }

        if (truncated && ell.valid && ell.w > 0.0f && (cursor_x - start_x) + ell_w <= avail_w_phys + 2.0f) {
            glyphs_.push_back(GlyphInstance{
                .x       = std::floor(cursor_x + ell.bearing_x),
                .y       = std::floor(baseline_phys - ell.bearing_y),
                .w       = ell.w,
                .h       = ell.h,
                .u0      = ell.u0,
                .v0      = ell.v0,
                .u1      = ell.u1,
                .v1      = ell.v1,
                .clip_x0 = cx0,
                .clip_y0 = cy0,
                .clip_x1 = cx1,
                .clip_y1 = cy1,
                .rgba    = rgba,
                .sx      = ell.sx,
                .sy      = ell.sy,
            });
            cursor_x += ell_w;
        }
        return cursor_x - start_x;
    }

    // Fast path: pure ASCII / Latin-1 directly from Arena string_view (ZERO allocations, ZERO HarfBuzz)
    const size_t n = text.size();
    float cursor_x = x_phys;
    bool truncated = false;
    const size_t glyph_start = glyphs_.size();
    float last_valid_x = cursor_x;
    size_t last_valid_glyph_count = glyph_start;

    for (size_t i = 0; i < n; ++i) {
        uint8_t b = static_cast<uint8_t>(text[i]);
        if ((cell_flags & core::kCellHasNewline) != 0) {
            if (b == '\r') {
                if (i + 1 < n && text[i + 1] == '\n') {
                    ++i;
                }
                b = static_cast<uint8_t>(' ');
            } else if (b == '\n' || b == '\t') {
                b = static_cast<uint8_t>(' ');
            }
        }
        const GlyphMetric& m = atlas.latin1(b);
        if (!m.valid) {
            continue;
        }
        if ((cursor_x - start_x) + m.advance_x <= avail_w_phys - ell_w) {
            last_valid_x = cursor_x + m.advance_x;
            last_valid_glyph_count = glyphs_.size() + ((m.w > 0.0f && m.h > 0.0f) ? 1u : 0u);
        }
        if ((cursor_x - start_x) + m.advance_x > avail_w_phys) {
            truncated = true;
            break;
        }
        if (m.w > 0.0f && m.h > 0.0f) {
            glyphs_.push_back(GlyphInstance{
                .x       = std::floor(cursor_x + m.bearing_x),
                .y       = std::floor(baseline_phys - m.bearing_y),
                .w       = m.w,
                .h       = m.h,
                .u0      = m.u0,
                .v0      = m.v0,
                .u1      = m.u1,
                .v1      = m.v1,
                .clip_x0 = cx0,
                .clip_y0 = cy0,
                .clip_x1 = cx1,
                .clip_y1 = cy1,
                .rgba    = rgba,
                .sx      = m.sx,
                .sy      = m.sy,
            });
        }
        cursor_x += m.advance_x;
    }

    if (truncated && ell.valid) {
        glyphs_.resize(last_valid_glyph_count);
        cursor_x = last_valid_x;
        if (ell.w > 0.0f && ell.h > 0.0f && (cursor_x - start_x) + ell_w <= avail_w_phys + 2.0f) {
            glyphs_.push_back(GlyphInstance{
                .x       = std::floor(cursor_x + ell.bearing_x),
                .y       = std::floor(baseline_phys - ell.bearing_y),
                .w       = ell.w,
                .h       = ell.h,
                .u0      = ell.u0,
                .v0      = ell.v0,
                .u1      = ell.u1,
                .v1      = ell.v1,
                .clip_x0 = cx0,
                .clip_y0 = cy0,
                .clip_x1 = cx1,
                .clip_y1 = cy1,
                .rgba    = rgba,
                .sx      = ell.sx,
                .sy      = ell.sy,
            });
            cursor_x += ell_w;
        }
    }
    return cursor_x - start_x;
}

void GridFrameBuilder::build(const GridFrameInput& in, GlyphAtlas& atlas) {
    rects_.clear();
    glyphs_.clear();

    if (in.viewport_w_dip <= 0 || in.viewport_h_dip <= 0) {
        return;
    }

    const float dpr = (std::max)(1.0f, in.dpr);
    atlas.ensure_initialized(dpr);

    const float w_phys       = std::round(static_cast<float>(in.viewport_w_dip) * dpr);
    const float h_phys       = std::round(static_cast<float>(in.viewport_h_dip) * dpr);
    const float border_px    = in.draw_outer_border ? std::max(1.0f, std::round(dpr)) : 0.0f;
    const float origin_x     = border_px;
    const float origin_y     = border_px;
    const float inner_w_phys = (std::max)(0.0f, w_phys - 2.0f * border_px);
    const float inner_h_phys = (std::max)(0.0f, h_phys - 2.0f * border_px);
    const float px1          = (std::max)(1.0f, std::round(dpr));

    // Global background (#F5F5F5)
    push_rect(0.0f, 0.0f, w_phys, h_phys, kColGridCellBg, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);

    if (in.storage == nullptr || in.view_state == nullptr || inner_w_phys <= 0.0f || inner_h_phys <= 0.0f) {
        if (in.draw_outer_border) {
            push_rect(0.0f, 0.0f, w_phys, border_px, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
            push_rect(0.0f, h_phys - border_px, w_phys, border_px, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
            push_rect(0.0f, 0.0f, border_px, h_phys, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
            push_rect(w_phys - border_px, 0.0f, border_px, h_phys, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
        }
        return;
    }

    const core::ResultSetStorage& rs = *in.storage;
    GridViewState& vs                = *in.view_state;
    const uint64_t total_rows        = rs.published_rows.load(std::memory_order_acquire);
    const auto& vis_ordinals         = vs.columns.visible_ordinals();
    const uint32_t vis_col_count     = static_cast<uint32_t>(vis_ordinals.size());

    const float row_h_phys      = std::round(kRowHeightDip * dpr);
    const float col_hdr_h_phys  = std::round(kColHeaderHeightDip * dpr);
    const float row_hdr_w_phys  = std::round(in.row_header_w_dip * dpr);
    const float data_x0         = origin_x + row_hdr_w_phys;
    const float data_y0         = origin_y + col_hdr_h_phys;
    const float data_x1         = origin_x + inner_w_phys;
    const float data_y1         = origin_y + inner_h_phys;
    const float data_w_dip      = (std::max)(0.0f, static_cast<float>(in.viewport_w_dip) - (in.draw_outer_border ? 2.0f : 0.0f) - in.row_header_w_dip);
    const float data_h_dip      = (std::max)(0.0f, static_cast<float>(in.viewport_h_dip) - (in.draw_outer_border ? 2.0f : 0.0f) - kColHeaderHeightDip);
    const float h_offset_dip    = static_cast<float>(std::round(vs.hscroll_px));
    const float baseline_off    = atlas.baseline_in_row(row_h_phys);
    const float pad_left_phys   = std::round(7.0f * dpr);
    const float pad_right_phys  = std::round(8.0f * dpr);
    const float rh_pad_r_phys   = std::round(4.0f * dpr);

    // Visible row & column ranges
    const int64_t first_row = (total_rows > 0) ? (std::clamp<int64_t>)(vs.vscroll_row, 0, static_cast<int64_t>(total_rows) - 1) : 0;
    const int64_t vis_rows  = static_cast<int64_t>(std::ceil(data_h_dip / kRowHeightDip));
    const int64_t last_row  = (total_rows > 0) ? (std::min<int64_t>)(static_cast<int64_t>(total_rows) - 1, first_row + vis_rows) : -1;

    const auto [first_vis_col, last_vis_col] = vs.columns.visible_range(h_offset_dip, data_w_dip);

    // -----------------------------------------------------------------------
    // Quadrant 1: Data Cells
    // -----------------------------------------------------------------------
    if (total_rows > 0 && first_row <= last_row && first_vis_col <= last_vis_col && data_x1 > data_x0 && data_y1 > data_y0) {
        const float first_col_left_phys = (std::max)(
            data_x0,
            data_x0 + std::round((vs.columns.column_left(static_cast<uint32_t>(first_vis_col)) - h_offset_dip) * dpr));
        const float last_col_right_phys = (std::min)(
            data_x1,
            data_x0 + std::round((vs.columns.column_right(static_cast<uint32_t>(last_vis_col)) - h_offset_dip) * dpr));
        const float row_lines_w = (std::max)(0.0f, last_col_right_phys - first_col_left_phys);
        const float total_rows_h = static_cast<float>(last_row - first_row + 1) * row_h_phys;

        for (int64_t r = first_row; r <= last_row; ++r) {
            const float row_top = data_y0 + static_cast<float>(r - first_row) * row_h_phys;
            const uint64_t phys_row = vs.resolve_physical_row(static_cast<uint64_t>(r));
            const auto [chunk, local_r] = rs.locate_row(phys_row);

            for (int32_t vc = first_vis_col; vc <= last_vis_col; ++vc) {
                const uint32_t ord = vis_ordinals[static_cast<size_t>(vc)];
                const GridColumnInfo& col = vs.columns.column(ord);
                const float col_left = data_x0 + std::round((vs.columns.column_left(static_cast<uint32_t>(vc)) - h_offset_dip) * dpr);
                const float col_w    = std::round(col.width * dpr);

                const bool has_cell = (chunk != nullptr && ord < chunk->col_count);
                const core::CellDesc desc = has_cell ? chunk->cell(local_r, ord) : core::CellDesc::make_null();
                const bool is_null = desc.is_null();
                const bool is_sel  = vs.selection.is_cell_selected(r, static_cast<int32_t>(ord));

                if (is_sel) {
                    push_rect(col_left, row_top, (std::max)(0.0f, col_w - px1), row_h_phys - px1,
                              kColGridSelectionBg, kRectSolid, data_x0, data_y0, data_x1, data_y1);
                } else if (is_null) {
                    push_rect(col_left, row_top, (std::max)(0.0f, col_w - px1), row_h_phys - px1,
                              kColGridNullBg, kRectSolid, data_x0, data_y0, data_x1, data_y1);
                }

                const float avail_w = col_w - pad_left_phys - pad_right_phys;
                if (avail_w > 1.0f) {
                    const std::string_view text = has_cell ? chunk->text_of(desc) : std::string_view{"NULL", 4};
                    bool is_link = false;
                    if (!is_null && in.display_json_as_link && col.may_hold_json &&
                        (desc.flags() & core::kCellJsonLike) != 0) {
                        is_link = vs.is_cached_json_doc(phys_row, ord, text);
                    }
                    const uint32_t text_rgba = is_link ? kColGridLinkFg : kColGridTextFg;
                    const float drawn_w = emit_text_fast(
                        atlas,
                        text,
                        is_null ? static_cast<uint8_t>(core::kCellNone) : desc.flags(),
                        col_left + pad_left_phys,
                        row_top + baseline_off,
                        avail_w,
                        text_rgba,
                        data_x0, data_y0, data_x1, data_y1);
                    if (is_link && drawn_w > 0.0f) {
                        const float ul_y = std::floor(row_top + baseline_off + std::max(1.0f, std::round(2.0f * dpr)));
                        push_rect(col_left + pad_left_phys, ul_y, drawn_w, px1,
                                  kColGridLinkFg, kRectSolid, data_x0, data_y0, data_x1, data_y1);
                    }
                }
            }

            // 1px horizontal grid line (#CCCEDB) stopping at last column
            if (row_lines_w > 0.0f) {
                push_rect(first_col_left_phys, row_top + row_h_phys - px1, row_lines_w, px1,
                          kColGridLine, kRectSolid, data_x0, data_y0, data_x1, data_y1);
            }
        }

        // 1px vertical grid lines (#CCCEDB) stopping at last visible row
        for (int32_t vc = first_vis_col; vc <= last_vis_col; ++vc) {
            const float col_right = data_x0 + std::round((vs.columns.column_right(static_cast<uint32_t>(vc)) - h_offset_dip) * dpr);
            push_rect(col_right - px1, data_y0, px1, total_rows_h,
                      kColGridLine, kRectSolid, data_x0, data_y0, data_x1, data_y1);
        }

        // Dotted focus rectangle (#000000) on CurrentCell (inset by 1px from right/bottom gridline)
        if (const auto& cur = vs.selection.current_cell(); cur.has_value() && cur->row >= first_row && cur->row <= last_row) {
            const int32_t cur_vc = vs.columns.visible_index_from_ordinal(static_cast<uint32_t>(cur->col_ordinal));
            if (cur_vc >= first_vis_col && cur_vc <= last_vis_col) {
                const float col_left = data_x0 + std::round((vs.columns.column_left(static_cast<uint32_t>(cur_vc)) - h_offset_dip) * dpr);
                const float col_w    = std::round(vs.columns.column(static_cast<uint32_t>(cur->col_ordinal)).width * dpr);
                const float row_top  = data_y0 + static_cast<float>(cur->row - first_row) * row_h_phys;
                if (col_w > 2.0f * px1) {
                    push_rect(col_left, row_top, col_w - px1, row_h_phys - px1,
                              kColGridFocusRect, kRectFocusDash, data_x0, data_y0, data_x1, data_y1);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Quadrant 2: Row Headers (left, fixed horizontally)
    // -----------------------------------------------------------------------
    if (total_rows > 0 && first_row <= last_row && row_hdr_w_phys > 0.0f && data_y1 > data_y0) {
        const float total_rows_h = static_cast<float>(last_row - first_row + 1) * row_h_phys;
        const float rh_x0 = origin_x;
        const float rh_x1 = data_x0;
        if (row_hdr_w_phys > px1 && total_rows_h > 0.0f) {
            push_rect(rh_x0, data_y0, row_hdr_w_phys - px1, total_rows_h,
                      kColGridHeaderBg, kRectSolid, rh_x0, data_y0, rh_x1, data_y1);
        }
        push_rect(rh_x1 - px1, data_y0, px1, total_rows_h,
                  kColGridHeaderDiv, kRectSolid, rh_x0, data_y0, rh_x1, data_y1);

        char num_buf[24]{};
        for (int64_t r = first_row; r <= last_row; ++r) {
            const float row_top = data_y0 + static_cast<float>(r - first_row) * row_h_phys;
            push_rect(rh_x0, row_top + row_h_phys - px1, row_hdr_w_phys, px1,
                      kColGridHeaderDiv, kRectSolid, rh_x0, data_y0, rh_x1, data_y1);

            const auto [ptr, ec] = std::to_chars(num_buf, num_buf + sizeof(num_buf), r + 1);
            if (ec == std::errc{}) {
                const std::string_view num_sv(num_buf, static_cast<size_t>(ptr - num_buf));
                const float avail_w = (std::max)(0.0f, row_hdr_w_phys - px1 - rh_pad_r_phys - 2.0f * dpr);
                const float text_w  = atlas.measure_fast(num_sv, 24) * dpr;
                const float text_x  = (std::max)(rh_x0 + 2.0f * dpr, rh_x1 - px1 - rh_pad_r_phys - (std::min)(text_w, avail_w));
                (void)emit_text_fast(
                    atlas,
                    num_sv,
                    core::kCellNone,
                    text_x,
                    row_top + baseline_off,
                    avail_w,
                    kColGridTextFg,
                    rh_x0, data_y0, rh_x1, data_y1);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Quadrant 3: Column Headers (top, fixed vertically)
    // -----------------------------------------------------------------------
    if (vis_col_count > 0 && first_vis_col <= last_vis_col && data_x1 > data_x0 && col_hdr_h_phys > 0.0f) {
        const float first_col_left = (std::max)(
            data_x0,
            data_x0 + std::round((vs.columns.column_left(static_cast<uint32_t>(first_vis_col)) - h_offset_dip) * dpr));
        const float last_col_right = (std::min)(
            data_x1,
            data_x0 + std::round((vs.columns.column_right(static_cast<uint32_t>(last_vis_col)) - h_offset_dip) * dpr));

        if (last_col_right > first_col_left) {
            if (col_hdr_h_phys > px1) {
                push_rect(first_col_left, origin_y, last_col_right - first_col_left, col_hdr_h_phys - px1,
                          kColGridHeaderBg, kRectSolid, data_x0, origin_y, data_x1, data_y0);
            }
            push_rect(first_col_left, data_y0 - px1, last_col_right - first_col_left, px1,
                      kColGridHeaderDiv, kRectSolid, data_x0, origin_y, data_x1, data_y0);
        }

        for (int32_t vc = first_vis_col; vc <= last_vis_col; ++vc) {
            const uint32_t ord = vis_ordinals[static_cast<size_t>(vc)];
            const GridColumnInfo& col = vs.columns.column(ord);
            const float col_left  = data_x0 + std::round((vs.columns.column_left(static_cast<uint32_t>(vc)) - h_offset_dip) * dpr);
            const float col_w     = std::round(col.width * dpr);
            const float col_right = col_left + col_w;

            push_rect(col_right - px1, origin_y, px1, col_hdr_h_phys,
                      kColGridHeaderDiv, kRectSolid, data_x0, origin_y, data_x1, data_y0);

            const bool has_sort = (col.sort_dir != SortDirection::None);
            const float arrow_space = has_sort ? std::round(11.0f * dpr) : 0.0f;
            const float avail_w = col_w - pad_left_phys - pad_right_phys - arrow_space;
            if (avail_w > 1.0f && !col.caption.empty()) {
                uint8_t hdr_flags = core::kCellNone;
                for (unsigned char c : col.caption) {
                    if (c >= 0x80u) {
                        hdr_flags |= core::kCellNonAscii;
                        break;
                    }
                }
                (void)emit_text_fast(
                    atlas,
                    col.caption,
                    hdr_flags,
                    col_left + pad_left_phys,
                    origin_y + baseline_off,
                    avail_w,
                    kColGridTextFg,
                    data_x0, origin_y, data_x1, data_y0);
            }

            if (has_sort && col.width >= 18.0f) {
                const float aw = std::round(7.0f * dpr);
                const float ah = std::round(4.0f * dpr);
                const float ax = std::round(col_right - 15.0f * dpr);
                const float ay = origin_y + std::round(7.0f * dpr);
                push_rect(ax, ay, aw, ah,
                          kColGridSortArrow,
                          col.sort_dir == SortDirection::Ascending ? kRectSortAsc : kRectSortDesc,
                          data_x0, origin_y, data_x1, data_y0);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Quadrant 4: Top-Left Select-All Corner
    // -----------------------------------------------------------------------
    if (row_hdr_w_phys > 0.0f && col_hdr_h_phys > 0.0f) {
        if (row_hdr_w_phys > px1 && col_hdr_h_phys > px1) {
            push_rect(origin_x, origin_y, row_hdr_w_phys - px1, col_hdr_h_phys - px1,
                      kColGridTopLeftBg, kRectSolid, origin_x, origin_y, data_x0, data_y0);
        }
        push_rect(data_x0 - px1, origin_y, px1, col_hdr_h_phys,
                  kColGridHeaderDiv, kRectSolid, origin_x, origin_y, data_x0, data_y0);
        push_rect(origin_x, data_y0 - px1, row_hdr_w_phys, px1,
                  kColGridHeaderDiv, kRectSolid, origin_x, origin_y, data_x0, data_y0);
    }

    // Optional 1px outer border (#ABADB3) for standalone raster renders
    if (in.draw_outer_border) {
        push_rect(0.0f, 0.0f, w_phys, border_px, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
        push_rect(0.0f, h_phys - border_px, w_phys, border_px, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
        push_rect(0.0f, 0.0f, border_px, h_phys, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
        push_rect(w_phys - border_px, 0.0f, border_px, h_phys, kColGridOuterBorder, kRectSolid, 0.0f, 0.0f, w_phys, h_phys);
    }
}

} // namespace capybaradb::ui::grid
