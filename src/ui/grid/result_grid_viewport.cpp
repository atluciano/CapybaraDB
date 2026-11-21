#include "ui/grid/result_grid_viewport.hpp"

#include "core/result/result_formatter.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace capybaradb::ui {

namespace detail {

class GridSurfaceWidget final : public QWidget {
public:
    explicit GridSurfaceWidget(ResultGridViewport* owner)
        : QWidget(owner)
        , owner_(owner) {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        owner_->on_surface_paint_software(p, width(), height(), static_cast<float>(devicePixelRatioF()));
    }
    void mousePressEvent(QMouseEvent* e) override { owner_->on_surface_mouse_press(e); }
    void mouseMoveEvent(QMouseEvent* e) override { owner_->on_surface_mouse_move(e); }
    void mouseReleaseEvent(QMouseEvent* e) override { owner_->on_surface_mouse_release(e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { owner_->on_surface_mouse_double_click(e); }
    void keyPressEvent(QKeyEvent* e) override { owner_->keyPressEvent(e); }
    void wheelEvent(QWheelEvent* e) override { owner_->wheelEvent(e); }
    void contextMenuEvent(QContextMenuEvent* e) override { owner_->show_context_menu(e->globalPos()); }

private:
    ResultGridViewport* owner_ = nullptr;
};

} // namespace detail

ResultGridViewport::ResultGridViewport(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("ResultGridViewport"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(0, 0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setStyleSheet(QStringLiteral(
        "QFrame#ResultGridViewport { background-color: #F5F5F5; border: 1px solid #ABADB3; }"));

    grid_layout_ = new QGridLayout(this);
    grid_layout_->setContentsMargins(0, 0, 0, 0);
    grid_layout_->setSpacing(0);

    v_scroll_ = new QScrollBar(Qt::Vertical, this);
    h_scroll_ = new QScrollBar(Qt::Horizontal, this);
    v_scroll_->setVisible(false);
    h_scroll_->setVisible(false);

    corner_filler_ = new QWidget(this);
    corner_filler_->setStyleSheet(QStringLiteral("background-color: #F0F0F0;"));
    corner_filler_->setVisible(false);

    create_surface();

    grid_layout_->addWidget(v_scroll_, 0, 1);
    grid_layout_->addWidget(h_scroll_, 1, 0);
    grid_layout_->addWidget(corner_filler_, 1, 1);
    grid_layout_->setRowStretch(0, 1);
    grid_layout_->setColumnStretch(0, 1);

    connect(v_scroll_, &QScrollBar::valueChanged, this, [this](int val) {
        if (updating_scroll_bars_ || view_state_ == nullptr) {
            return;
        }
        view_state_->vscroll_row = (std::max<int64_t>)(0, val);
        update_surface();
    });

    connect(h_scroll_, &QScrollBar::valueChanged, this, [this](int val) {
        if (updating_scroll_bars_ || view_state_ == nullptr) {
            return;
        }
        view_state_->hscroll_px = static_cast<double>((std::max)(0, val));
        update_surface();
    });

    auto_scroll_timer_ = new QTimer(this);
    auto_scroll_timer_->setInterval(16);
    connect(auto_scroll_timer_, &QTimer::timeout, this, &ResultGridViewport::on_auto_scroll_tick);
}

ResultGridViewport::~ResultGridViewport() {
    if (auto_scroll_timer_ != nullptr) {
        auto_scroll_timer_->stop();
    }
}

void ResultGridViewport::create_surface() {
    surface_ = new detail::GridSurfaceWidget(this);
    surface_->setMinimumSize(0, 0);
    surface_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    grid_layout_->addWidget(surface_, 0, 0);
    setFocusProxy(surface_);
}

void ResultGridViewport::update_surface() {
    if (surface_ != nullptr) {
        surface_->update();
    }
}

void ResultGridViewport::set_display_json_as_link(bool enabled) {
    if (display_json_as_link_ == enabled) {
        return;
    }
    display_json_as_link_ = enabled;
    update_surface();
}

void ResultGridViewport::set_active_result_set(
    const core::ResultSetStorage* storage,
    grid::GridViewState* view_state) {
    storage_    = storage;
    view_state_ = view_state;

    if (storage_ != nullptr && view_state_ != nullptr) {
        if (!view_state_->columns_initialized) {
            if (view_state_->columns.column_count() != storage_->col_count) {
                view_state_->columns.init_from_storage(*storage_);
            }
            view_state_->columns_initialized = true;
        }
        notify_rows_available();
    } else {
        row_header_w_dip_ = grid::kMinRowHeaderWidthDip;
        update_scroll_bars();
        update_surface();
    }
}

void ResultGridViewport::clear() {
    storage_    = nullptr;
    view_state_ = nullptr;
    row_header_w_dip_ = grid::kMinRowHeaderWidthDip;
    update_scroll_bars();
    update_surface();
}

void ResultGridViewport::notify_rows_available() {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    if (!view_state_->columns_initialized) {
        if (view_state_->columns.column_count() != storage_->col_count) {
            view_state_->columns.init_from_storage(*storage_);
        }
        view_state_->columns_initialized = true;
    }

    const uint64_t nrows = storage_->published_rows.load(std::memory_order_acquire);
    row_header_w_dip_ = grid::compute_row_header_width_dip(nrows);

    // Arrival of 1st row: position CurrentCell=(0,0) with dotted focus rect WITHOUT selecting (PARITY_SPEC §3.2)
    if (nrows > 0 && storage_->col_count > 0 && !view_state_->first_row_focused) {
        view_state_->first_row_focused = true;
        if (!view_state_->selection.current_cell().has_value()) {
            const auto& vis = view_state_->columns.visible_ordinals();
            const int32_t first_col = vis.empty() ? 0 : static_cast<int32_t>(vis[0]);
            view_state_->selection.set_current_cell(grid::GridCellPos{0, first_col});
            view_state_->selection.set_anchor_cell(grid::GridCellPos{0, first_col});
        }
    }

    sample_rows_for_auto_size();
    update_scroll_bars();
    update_surface();
}

grid::GlyphAtlas& ResultGridViewport::ensure_atlas(float dpr) {
    const float clamped = (std::max)(1.0f, (std::min)(4.0f, dpr));
    if (atlas_ == nullptr || std::abs(atlas_->dpr() - clamped) >= 0.01f) {
        atlas_ = grid::glyph_atlas_for_dpr(clamped);
    }
    return *atlas_;
}

grid::GlyphAtlas& ResultGridViewport::glyph_atlas() {
    return ensure_atlas(static_cast<float>(devicePixelRatioF()));
}

void ResultGridViewport::sample_rows_for_auto_size() {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    const uint32_t col_count = storage_->col_count;
    if (col_count == 0) {
        return;
    }
    grid::GlyphAtlas& atlas = ensure_atlas(static_cast<float>(devicePixelRatioF()));

    bool changed = false;
    for (uint32_t c = 0; c < col_count; ++c) {
        auto& col = view_state_->columns.column(c);
        if (col.user_resized || col.header_measured) {
            continue;
        }
        const float hdr_w = std::ceil(atlas.measure_fast(col.caption, 64) + 20.0f);
        col.width = std::clamp(hdr_w, grid::ColumnMetrics::kDefaultMinAutoWidth, grid::ColumnMetrics::kDefaultMaxAutoWidth);
        col.header_measured = true;
        changed = true;
    }

    const uint64_t nrows = storage_->published_rows.load(std::memory_order_acquire);
    if (nrows > 0) {
        const uint64_t max_sample = static_cast<uint64_t>(std::clamp<uint32_t>(4096u / (std::max<uint32_t>)(1u, col_count), 10u, 100u));
        const uint64_t limit = (std::min)(nrows, max_sample);
        if (view_state_->auto_sized_rows < limit) {
            for (uint64_t r = view_state_->auto_sized_rows; r < limit; ++r) {
                const auto [chunk, lr] = storage_->locate_row(r);
                if (chunk == nullptr) {
                    continue;
                }
                for (uint32_t c = 0; c < col_count && c < chunk->col_count; ++c) {
                    auto& col = view_state_->columns.column(c);
                    if (col.user_resized || col.width >= grid::ColumnMetrics::kDefaultMaxAutoWidth) {
                        continue;
                    }
                    const std::string_view text = chunk->text_of(chunk->cell(lr, c));
                    const float cell_w = std::ceil(atlas.measure_fast(text, 64) + 16.0f);
                    if (cell_w > col.width) {
                        col.width = std::clamp(cell_w, grid::ColumnMetrics::kDefaultMinAutoWidth, grid::ColumnMetrics::kDefaultMaxAutoWidth);
                        changed = true;
                    }
                }
            }
            view_state_->auto_sized_rows = limit;
        }
    }

    if (changed) {
        view_state_->columns.invalidate();
    }
}

void ResultGridViewport::auto_fit_column(int32_t visible_col_idx) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    const auto& vis = view_state_->columns.visible_ordinals();
    if (visible_col_idx < 0 || static_cast<size_t>(visible_col_idx) >= vis.size()) {
        return;
    }
    grid::GlyphAtlas& atlas = ensure_atlas(static_cast<float>(devicePixelRatioF()));

    const uint32_t ord = vis[static_cast<size_t>(visible_col_idx)];
    const auto& col = view_state_->columns.column(ord);
    float max_w = atlas.measure_fast(col.caption, 128) + 24.0f;

    const uint64_t nrows = storage_->published_rows.load(std::memory_order_acquire);
    if (nrows > 0) {
        const int64_t start = (std::max<int64_t>)(0, view_state_->vscroll_row - 50);
        const int64_t end   = (std::min<int64_t>)(static_cast<int64_t>(nrows), start + 300);
        for (int64_t r = start; r < end; ++r) {
            const uint64_t phys = view_state_->resolve_physical_row(static_cast<uint64_t>(r));
            const auto [chunk, lr] = storage_->locate_row(phys);
            if (chunk == nullptr || ord >= chunk->col_count) {
                continue;
            }
            const float w = atlas.measure_fast(chunk->text_of(chunk->cell(lr, ord)), 128) + 16.0f;
            if (w > max_w) {
                max_w = w;
            }
        }
    }

    const float fitted = std::clamp(std::ceil(max_w), grid::ColumnMetrics::kAutoFitMinWidth, grid::ColumnMetrics::kAutoFitMaxWidth);
    view_state_->columns.set_column_width(ord, fitted, true);
    update_scroll_bars();
    update_surface();
}

int64_t ResultGridViewport::total_rows() const noexcept {
    if (storage_ == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(storage_->published_rows.load(std::memory_order_acquire));
}

double ResultGridViewport::data_viewport_w_dip() const noexcept {
    const int sw = (surface_ != nullptr) ? surface_->width() : width();
    return (std::max)(0.0, static_cast<double>(sw) - static_cast<double>(row_header_w_dip_));
}

double ResultGridViewport::data_viewport_h_dip() const noexcept {
    const int sh = (surface_ != nullptr) ? surface_->height() : height();
    return (std::max)(0.0, static_cast<double>(sh) - static_cast<double>(grid::kColHeaderHeightDip));
}

int64_t ResultGridViewport::full_visible_rows() const noexcept {
    return (std::max<int64_t>)(1, static_cast<int64_t>(std::floor(data_viewport_h_dip() / grid::kRowHeightDip)));
}

void ResultGridViewport::update_scroll_bars() {
    const int64_t nrows = total_rows();
    const double total_cols_w = (view_state_ != nullptr) ? static_cast<double>(view_state_->columns.total_width()) : 0.0;

    const int sb_extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    double avail_w = (std::max)(0.0, static_cast<double>(width() - 2) - static_cast<double>(row_header_w_dip_));
    double avail_h = (std::max)(0.0, static_cast<double>(height() - 2) - static_cast<double>(grid::kColHeaderHeightDip));

    bool need_v = (static_cast<double>(nrows) * grid::kRowHeightDip) > (avail_h + 0.5);
    bool need_h = total_cols_w > (avail_w + 0.5);

    if (need_v) {
        avail_w = (std::max)(0.0, avail_w - sb_extent);
    }
    if (need_h && !need_v &&
        (static_cast<double>(nrows) * grid::kRowHeightDip) > ((std::max)(0.0, avail_h - sb_extent) + 0.5)) {
        need_v = true;
        avail_w = (std::max)(0.0, avail_w - sb_extent);
    }
    if (need_v && !need_h && total_cols_w > (avail_w + 0.5)) {
        need_h = true;
    }
    if (need_h) {
        avail_h = (std::max)(0.0, avail_h - sb_extent);
    }

    v_scroll_->setVisible(need_v);
    h_scroll_->setVisible(need_h);
    corner_filler_->setVisible(need_v && need_h);

    const int64_t full_rows = (std::max<int64_t>)(1, static_cast<int64_t>(std::floor(avail_h / grid::kRowHeightDip)));
    const int64_t max_row_off = (std::max<int64_t>)(0, nrows - full_rows);
    const int max_h_off = static_cast<int>(std::ceil((std::max)(0.0, total_cols_w - avail_w)));

    if (view_state_ != nullptr) {
        view_state_->vscroll_row = std::clamp<int64_t>(view_state_->vscroll_row, 0, max_row_off);
        view_state_->hscroll_px  = std::clamp<double>(view_state_->hscroll_px, 0.0, static_cast<double>(max_h_off));
    }

    updating_scroll_bars_ = true;
    v_scroll_->setRange(0, static_cast<int>((std::min<int64_t>)(max_row_off, INT_MAX)));
    v_scroll_->setPageStep(static_cast<int>((std::max<int64_t>)(1, full_rows - 1)));
    v_scroll_->setSingleStep(1);
    v_scroll_->setValue(view_state_ != nullptr ? static_cast<int>(view_state_->vscroll_row) : 0);

    h_scroll_->setRange(0, max_h_off);
    h_scroll_->setPageStep(static_cast<int>((std::max)(24.0, avail_w * 0.8)));
    h_scroll_->setSingleStep(24);
    h_scroll_->setValue(view_state_ != nullptr ? static_cast<int>(std::round(view_state_->hscroll_px)) : 0);
    updating_scroll_bars_ = false;
}

void ResultGridViewport::ensure_cell_visible(int64_t row, int32_t col_ordinal) {
    if (view_state_ == nullptr || storage_ == nullptr) {
        return;
    }
    const int64_t nrows = total_rows();
    if (nrows > 0) {
        row = std::clamp<int64_t>(row, 0, nrows - 1);
        const int64_t full_rows = full_visible_rows();
        if (row < view_state_->vscroll_row) {
            view_state_->vscroll_row = row;
        } else if (row >= view_state_->vscroll_row + full_rows) {
            view_state_->vscroll_row = (std::max<int64_t>)(0, row - full_rows + 1);
        }
    }

    const int32_t vc = view_state_->columns.visible_index_from_ordinal(static_cast<uint32_t>(col_ordinal));
    if (vc >= 0) {
        const double col_l = static_cast<double>(view_state_->columns.column_left(static_cast<uint32_t>(vc)));
        const double col_r = static_cast<double>(view_state_->columns.column_right(static_cast<uint32_t>(vc)));
        const double vp_w  = data_viewport_w_dip();
        if (col_l < view_state_->hscroll_px) {
            view_state_->hscroll_px = col_l;
        } else if (col_r > view_state_->hscroll_px + vp_w) {
            view_state_->hscroll_px = (std::max)(0.0, col_r - vp_w);
        }
    }

    update_scroll_bars();
    update_surface();
}

bool ResultGridViewport::has_selection() const noexcept {
    return view_state_ != nullptr && view_state_->selection.has_selection();
}

void ResultGridViewport::select_all() {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    view_state_->selection.select_all(total_rows(), static_cast<int32_t>(storage_->col_count));
    update_surface();
    emit selection_changed();
}

bool ResultGridViewport::copy_selection(bool include_headers) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        emit status_message(QStringLiteral("Select a result grid first."));
        return false;
    }
    core::ClipboardCopyOptions opts;
    opts.ranges             = view_state_->selection.ranges();
    opts.display_order      = view_state_->columns.visible_ordinals();
    opts.row_order          = view_state_->row_order;
    opts.include_headers    = include_headers;
    opts.retain_line_breaks = retain_line_breaks_;

    const auto tsv = core::build_clipboard_tsv(*storage_, opts);
    if (!tsv.has_value()) {
        emit status_message(QString::fromStdString(tsv.error()));
        return false;
    }
    if (QClipboard* cb = QApplication::clipboard()) {
        cb->setText(QString::fromUtf8(tsv->data(), static_cast<qsizetype>(tsv->size())));
    }
    emit status_message(include_headers ? QStringLiteral("Copied with headers.") : QStringLiteral("Copied selection."));
    return true;
}

void ResultGridViewport::on_surface_paint_software(QPainter& painter, int w_dip, int h_dip, float dpr) {
    grid::GlyphAtlas& atlas = ensure_atlas(dpr);
    grid::GridFrameInput in;
    in.storage              = storage_;
    in.view_state           = view_state_;
    in.viewport_w_dip       = w_dip;
    in.viewport_h_dip       = h_dip;
    in.dpr                  = dpr;
    in.row_header_w_dip     = row_header_w_dip_;
    in.display_json_as_link = display_json_as_link_;
    in.draw_outer_border    = false;

    frame_builder_.build(in, atlas);
    const int w_phys = static_cast<int>(std::round(static_cast<float>(w_dip) * dpr));
    const int h_phys = static_cast<int>(std::round(static_cast<float>(h_dip) * dpr));
    sw_backend_.render(w_phys, h_phys, frame_builder_.rects(), frame_builder_.glyphs(), atlas);

    QImage img = sw_backend_.image();
    img.setDevicePixelRatio(static_cast<qreal>(dpr));
    painter.drawImage(0, 0, img);
}

QImage ResultGridViewport::render_software_frame(
    int width_dip,
    int height_dip,
    float dpr,
    bool draw_outer_border) {
    grid::GlyphAtlas& atlas = ensure_atlas(dpr);
    grid::GridFrameInput in;
    in.storage              = storage_;
    in.view_state           = view_state_;
    in.viewport_w_dip       = width_dip;
    in.viewport_h_dip       = height_dip;
    in.dpr                  = dpr;
    in.row_header_w_dip     = row_header_w_dip_;
    in.display_json_as_link = display_json_as_link_;
    in.draw_outer_border    = draw_outer_border;

    frame_builder_.build(in, atlas);
    const int w_phys = static_cast<int>(std::round(static_cast<float>(width_dip) * dpr));
    const int h_phys = static_cast<int>(std::round(static_cast<float>(height_dip) * dpr));
    sw_backend_.render(w_phys, h_phys, frame_builder_.rects(), frame_builder_.glyphs(), atlas);
    return sw_backend_.image().copy();
}

bool ResultGridViewport::hit_test_data_cell(const QPointF& pos, int64_t& row_out, int32_t& col_ord_out) const {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return false;
    }
    const int64_t nrows = total_rows();
    if (nrows <= 0 || view_state_->columns.visible_count() == 0) {
        return false;
    }
    const double rel_y = pos.y() - static_cast<double>(grid::kColHeaderHeightDip);
    const double rel_x = pos.x() - static_cast<double>(row_header_w_dip_) + view_state_->hscroll_px;
    if (rel_y < 0.0 || rel_x < 0.0) {
        return false;
    }
    const int64_t r = view_state_->vscroll_row + static_cast<int64_t>(std::floor(rel_y / grid::kRowHeightDip));
    if (r < 0 || r >= nrows) {
        return false;
    }
    const int32_t vc = view_state_->columns.hit_test_column(static_cast<float>(rel_x));
    if (vc < 0) {
        return false;
    }
    row_out     = r;
    col_ord_out = static_cast<int32_t>(view_state_->columns.visible_ordinals()[static_cast<size_t>(vc)]);
    return true;
}

void ResultGridViewport::on_surface_mouse_press(QMouseEvent* event) {
    if (surface_ != nullptr) {
        surface_->setFocus(Qt::MouseFocusReason);
    }
    if (event->button() != Qt::LeftButton || storage_ == nullptr || view_state_ == nullptr) {
        return;
    }

    const QPointF pos = event->position();
    last_mouse_pos_ = pos;
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool shift = (mods & Qt::ShiftModifier) != 0;
    const bool ctrl  = (mods & Qt::ControlModifier) != 0;

    // 1. Top-Left Select-All corner
    if (pos.x() < row_header_w_dip_ && pos.y() < grid::kColHeaderHeightDip) {
        select_all();
        return;
    }

    // 2. Column Headers (divider resize or sort click)
    if (pos.y() < grid::kColHeaderHeightDip && pos.x() >= row_header_w_dip_) {
        const float content_x = static_cast<float>(pos.x() - row_header_w_dip_ + view_state_->hscroll_px);
        const int32_t div_vc = view_state_->columns.hit_test_divider(content_x);
        if (div_vc >= 0) {
            const uint32_t ord = view_state_->columns.visible_ordinals()[static_cast<size_t>(div_vc)];
            active_drag_      = DragKind::ColumnResize;
            resizing_vis_col_ = div_vc;
            resize_start_x_   = pos.x();
            resize_start_w_   = view_state_->columns.column(ord).width;
            surface_->setCursor(Qt::SplitHCursor);
            return;
        }
        const int32_t vc = view_state_->columns.hit_test_column(content_x);
        if (vc >= 0) {
            active_drag_            = DragKind::ColumnHeaderClick;
            pressed_header_vis_col_ = vc;
        }
        return;
    }

    const int64_t nrows = total_rows();
    const int32_t ncols = static_cast<int32_t>(storage_->col_count);
    if (nrows <= 0 || ncols <= 0) {
        return;
    }

    // 3. Row Headers (select entire rows)
    if (pos.x() < row_header_w_dip_ && pos.y() >= grid::kColHeaderHeightDip) {
        const int64_t r = view_state_->vscroll_row +
            static_cast<int64_t>(std::floor((pos.y() - grid::kColHeaderHeightDip) / grid::kRowHeightDip));
        if (r < 0 || r >= nrows) {
            return;
        }
        active_drag_   = DragKind::RowHeaders;
        additive_drag_ = ctrl;
        if (shift && view_state_->selection.anchor_cell().has_value()) {
            const int64_t ar = view_state_->selection.anchor_cell()->row;
            view_state_->selection.select_range(ar, 0, r, ncols - 1, false);
        } else if (ctrl) {
            view_state_->selection.start_additive_range(r, 0);
            view_state_->selection.select_range(r, 0, r, ncols - 1, true);
        } else {
            view_state_->selection.set_anchor_cell(grid::GridCellPos{r, 0});
            view_state_->selection.select_range(r, 0, r, ncols - 1, false);
        }
        auto_scroll_timer_->start();
        update_surface();
        emit selection_changed();
        return;
    }

    // 4. Data Cells
    int64_t r = 0;
    int32_t ord = 0;
    if (!hit_test_data_cell(pos, r, ord)) {
        return;
    }

    active_drag_            = DragKind::Cells;
    additive_drag_          = ctrl;
    has_pressed_cell_       = (!shift && !ctrl);
    pressed_cell_           = grid::GridCellPos{r, ord};
    drag_left_initial_cell_ = false;

    if (shift && view_state_->selection.anchor_cell().has_value()) {
        const auto anc = *view_state_->selection.anchor_cell();
        view_state_->selection.select_range(anc.row, anc.col_ordinal, r, ord, false);
    } else if (ctrl) {
        view_state_->selection.start_additive_range(r, ord);
    } else {
        view_state_->selection.select_single_cell(r, ord);
    }

    auto_scroll_timer_->start();
    update_surface();
    emit selection_changed();
}

void ResultGridViewport::on_surface_mouse_move(QMouseEvent* event) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    const QPointF pos = event->position();
    last_mouse_pos_ = pos;

    if (active_drag_ == DragKind::None) {
        if (pos.y() < grid::kColHeaderHeightDip && pos.x() >= row_header_w_dip_) {
            const float content_x = static_cast<float>(pos.x() - row_header_w_dip_ + view_state_->hscroll_px);
            const int32_t div_vc = view_state_->columns.hit_test_divider(content_x);
            surface_->setCursor(div_vc >= 0 ? Qt::SplitHCursor : Qt::ArrowCursor);
        } else {
            surface_->setCursor(Qt::ArrowCursor);
        }
        return;
    }

    if (active_drag_ == DragKind::ColumnResize && resizing_vis_col_ >= 0) {
        const auto& vis = view_state_->columns.visible_ordinals();
        if (static_cast<size_t>(resizing_vis_col_) < vis.size()) {
            const uint32_t ord = vis[static_cast<size_t>(resizing_vis_col_)];
            const float next_w = resize_start_w_ + static_cast<float>(pos.x() - resize_start_x_);
            view_state_->columns.set_column_width(ord, next_w, true);
            update_scroll_bars();
            update_surface();
        }
        return;
    }

    const int64_t nrows = total_rows();
    const int32_t ncols = static_cast<int32_t>(storage_->col_count);
    if (nrows <= 0 || ncols <= 0) {
        return;
    }

    if (active_drag_ == DragKind::RowHeaders && view_state_->selection.anchor_cell().has_value()) {
        const int64_t raw_r = view_state_->vscroll_row +
            static_cast<int64_t>(std::floor((pos.y() - grid::kColHeaderHeightDip) / grid::kRowHeightDip));
        const int64_t r = std::clamp<int64_t>(raw_r, 0, nrows - 1);
        const int64_t ar = view_state_->selection.anchor_cell()->row;
        view_state_->selection.select_range(ar, 0, r, ncols - 1, additive_drag_);
        update_surface();
        emit selection_changed();
        return;
    }

    if (active_drag_ == DragKind::Cells && view_state_->selection.anchor_cell().has_value()) {
        const int64_t raw_r = view_state_->vscroll_row +
            static_cast<int64_t>(std::floor((pos.y() - grid::kColHeaderHeightDip) / grid::kRowHeightDip));
        const int64_t r = std::clamp<int64_t>(raw_r, 0, nrows - 1);
        const float content_x = std::clamp<float>(
            static_cast<float>(pos.x() - row_header_w_dip_ + view_state_->hscroll_px),
            0.0f,
            (std::max)(0.0f, view_state_->columns.total_width() - 1.0f));
        const int32_t vc = view_state_->columns.hit_test_column(content_x);
        if (vc >= 0) {
            const int32_t ord = static_cast<int32_t>(view_state_->columns.visible_ordinals()[static_cast<size_t>(vc)]);
            if (r != pressed_cell_.row || ord != pressed_cell_.col_ordinal) {
                drag_left_initial_cell_ = true;
            }
            const auto anc = *view_state_->selection.anchor_cell();
            view_state_->selection.select_range(anc.row, anc.col_ordinal, r, ord, additive_drag_);
            update_surface();
            emit selection_changed();
        }
    }
}

void ResultGridViewport::on_surface_mouse_release(QMouseEvent* event) {
    if (auto_scroll_timer_->isActive()) {
        auto_scroll_timer_->stop();
    }
    const DragKind finished = active_drag_;
    active_drag_ = DragKind::None;

    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }

    if (finished == DragKind::ColumnResize) {
        resizing_vis_col_ = -1;
        surface_->setCursor(Qt::ArrowCursor);
        return;
    }

    if (finished == DragKind::ColumnHeaderClick && pressed_header_vis_col_ >= 0) {
        const QPointF pos = event->position();
        if (pos.y() >= 0.0 && pos.y() < grid::kColHeaderHeightDip && pos.x() >= row_header_w_dip_) {
            const float content_x = static_cast<float>(pos.x() - row_header_w_dip_ + view_state_->hscroll_px);
            const int32_t vc = view_state_->columns.hit_test_column(content_x);
            if (vc == pressed_header_vis_col_) {
                const uint32_t ord = view_state_->columns.visible_ordinals()[static_cast<size_t>(vc)];
                view_state_->toggle_sort(*storage_, ord);
                update_surface();
            }
        }
        pressed_header_vis_col_ = -1;
        return;
    }

    // Simple left-click on a valid JSON link cell -> emit pretty-printed JSON
    if (finished == DragKind::Cells && has_pressed_cell_ && !drag_left_initial_cell_ && display_json_as_link_) {
        int64_t r = 0;
        int32_t ord = 0;
        if (hit_test_data_cell(event->position(), r, ord) &&
            r == pressed_cell_.row && ord == pressed_cell_.col_ordinal) {
            const auto& col = view_state_->columns.column(static_cast<uint32_t>(ord));
            if (col.may_hold_json) {
                const uint64_t phys = view_state_->resolve_physical_row(static_cast<uint64_t>(r));
                const auto [chunk, lr] = storage_->locate_row(phys);
                if (chunk != nullptr && static_cast<uint32_t>(ord) < chunk->col_count) {
                    const core::CellDesc& desc = chunk->cell(lr, static_cast<uint32_t>(ord));
                    if (!desc.is_null() && (desc.flags() & core::kCellJsonLike) != 0) {
                        const std::string_view raw = chunk->text_of(desc);
                        if (const auto pretty = core::pretty_print_json(raw)) {
                            emit json_link_clicked(QString::fromUtf8(pretty->data(), static_cast<qsizetype>(pretty->size())));
                        }
                    }
                }
            }
        }
    }
    has_pressed_cell_ = false;
}

void ResultGridViewport::on_surface_mouse_double_click(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    const QPointF pos = event->position();
    if (pos.y() < grid::kColHeaderHeightDip && pos.x() >= row_header_w_dip_) {
        const float content_x = static_cast<float>(pos.x() - row_header_w_dip_ + view_state_->hscroll_px);
        const int32_t div_vc = view_state_->columns.hit_test_divider(content_x);
        if (div_vc >= 0) {
            active_drag_ = DragKind::None;
            auto_fit_column(div_vc);
        }
    }
}

void ResultGridViewport::on_auto_scroll_tick() {
    if ((active_drag_ != DragKind::Cells && active_drag_ != DragKind::RowHeaders) ||
        storage_ == nullptr || view_state_ == nullptr || surface_ == nullptr) {
        auto_scroll_timer_->stop();
        return;
    }

    bool scrolled = false;
    const double sy = last_mouse_pos_.y();
    const double sx = last_mouse_pos_.x();
    const double surf_h = static_cast<double>(surface_->height());
    const double surf_w = static_cast<double>(surface_->width());

    if (sy < grid::kColHeaderHeightDip) {
        const int64_t step = (std::max<int64_t>)(1, static_cast<int64_t>((grid::kColHeaderHeightDip - sy) / 16.0));
        view_state_->vscroll_row = (std::max<int64_t>)(0, view_state_->vscroll_row - step);
        scrolled = true;
    } else if (sy > surf_h) {
        const int64_t step = (std::max<int64_t>)(1, static_cast<int64_t>((sy - surf_h) / 16.0));
        const int64_t max_r = (std::max<int64_t>)(0, total_rows() - full_visible_rows());
        view_state_->vscroll_row = (std::min<int64_t>)(max_r, view_state_->vscroll_row + step);
        scrolled = true;
    }

    if (active_drag_ == DragKind::Cells) {
        if (sx < row_header_w_dip_) {
            const double delta = (std::max)(8.0, (row_header_w_dip_ - sx) * 0.5);
            view_state_->hscroll_px = (std::max)(0.0, view_state_->hscroll_px - delta);
            scrolled = true;
        } else if (sx > surf_w) {
            const double delta = (std::max)(8.0, (sx - surf_w) * 0.5);
            const double max_h = (std::max)(0.0, static_cast<double>(view_state_->columns.total_width()) - data_viewport_w_dip());
            view_state_->hscroll_px = (std::min)(max_h, view_state_->hscroll_px + delta);
            scrolled = true;
        }
    }

    if (scrolled) {
        update_scroll_bars();
        update_surface();
    }
}

void ResultGridViewport::move_current_cell(int64_t row, int32_t col_ordinal, bool extend_selection) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        return;
    }
    const int64_t nrows = total_rows();
    if (nrows <= 0 || storage_->col_count == 0) {
        return;
    }
    row = std::clamp<int64_t>(row, 0, nrows - 1);
    col_ordinal = std::clamp<int32_t>(col_ordinal, 0, static_cast<int32_t>(storage_->col_count) - 1);

    if (extend_selection) {
        if (!view_state_->selection.anchor_cell().has_value()) {
            view_state_->selection.set_anchor_cell(view_state_->selection.current_cell().value_or(grid::GridCellPos{0, 0}));
        }
        const auto anc = *view_state_->selection.anchor_cell();
        view_state_->selection.select_range(anc.row, anc.col_ordinal, row, col_ordinal, false);
    } else {
        view_state_->selection.select_single_cell(row, col_ordinal);
    }

    ensure_cell_visible(row, col_ordinal);
    emit selection_changed();
}

void ResultGridViewport::keyPressEvent(QKeyEvent* event) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        QFrame::keyPressEvent(event);
        return;
    }

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl  = (mods & Qt::ControlModifier) != 0;
    const bool shift = (mods & Qt::ShiftModifier) != 0;

    if (ctrl && key == Qt::Key_A) {
        select_all();
        event->accept();
        return;
    }
    if (ctrl && key == Qt::Key_C) {
        (void)copy_selection(shift);
        event->accept();
        return;
    }

    const int64_t nrows = total_rows();
    const auto& vis = view_state_->columns.visible_ordinals();
    if (nrows <= 0 || vis.empty()) {
        QFrame::keyPressEvent(event);
        return;
    }

    const grid::GridCellPos cur = view_state_->selection.current_cell().value_or(
        grid::GridCellPos{0, static_cast<int32_t>(vis[0])});
    int32_t cur_vc = view_state_->columns.visible_index_from_ordinal(static_cast<uint32_t>(cur.col_ordinal));
    if (cur_vc < 0) {
        cur_vc = 0;
    }

    switch (key) {
    case Qt::Key_Up:
        move_current_cell(cur.row - 1, cur.col_ordinal, shift);
        event->accept();
        return;
    case Qt::Key_Down:
        move_current_cell(cur.row + 1, cur.col_ordinal, shift);
        event->accept();
        return;
    case Qt::Key_Left: {
        const int32_t next_vc = (std::max<int32_t>)(0, cur_vc - 1);
        move_current_cell(cur.row, static_cast<int32_t>(vis[static_cast<size_t>(next_vc)]), shift);
        event->accept();
        return;
    }
    case Qt::Key_Right: {
        const int32_t next_vc = (std::min<int32_t>)(static_cast<int32_t>(vis.size()) - 1, cur_vc + 1);
        move_current_cell(cur.row, static_cast<int32_t>(vis[static_cast<size_t>(next_vc)]), shift);
        event->accept();
        return;
    }
    case Qt::Key_PageUp:
        move_current_cell(cur.row - full_visible_rows(), cur.col_ordinal, shift);
        event->accept();
        return;
    case Qt::Key_PageDown:
        move_current_cell(cur.row + full_visible_rows(), cur.col_ordinal, shift);
        event->accept();
        return;
    case Qt::Key_Home:
        if (ctrl) {
            move_current_cell(0, static_cast<int32_t>(vis.front()), shift);
        } else {
            move_current_cell(cur.row, static_cast<int32_t>(vis.front()), shift);
        }
        event->accept();
        return;
    case Qt::Key_End:
        if (ctrl) {
            move_current_cell(nrows - 1, static_cast<int32_t>(vis.back()), shift);
        } else {
            move_current_cell(cur.row, static_cast<int32_t>(vis.back()), shift);
        }
        event->accept();
        return;
    default:
        break;
    }

    QFrame::keyPressEvent(event);
}

void ResultGridViewport::wheelEvent(QWheelEvent* event) {
    if (storage_ == nullptr || view_state_ == nullptr) {
        QFrame::wheelEvent(event);
        return;
    }

    const QPoint angle = event->angleDelta();
    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

    if (shift || (angle.x() != 0 && angle.y() == 0)) {
        const int delta = (angle.x() != 0) ? angle.x() : angle.y();
        const double max_h = (std::max)(0.0, static_cast<double>(view_state_->columns.total_width()) - data_viewport_w_dip());
        view_state_->hscroll_px = std::clamp(view_state_->hscroll_px - static_cast<double>(delta) * 0.5, 0.0, max_h);
    } else if (angle.y() != 0) {
        const int lines = (std::max)(1, QApplication::wheelScrollLines());
        const double notches = static_cast<double>(angle.y()) / 120.0;
        const int64_t step = static_cast<int64_t>(std::round(-notches * static_cast<double>(lines)));
        const int64_t max_r = (std::max<int64_t>)(0, total_rows() - full_visible_rows());
        view_state_->vscroll_row = std::clamp<int64_t>(view_state_->vscroll_row + step, 0, max_r);
    }

    update_scroll_bars();
    update_surface();
    event->accept();
}

void ResultGridViewport::show_context_menu(const QPoint& global_pos) {
    QMenu menu(this);
    QAction* act_copy = menu.addAction(QStringLiteral("Copy"));
    act_copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+C")));
    act_copy->setEnabled(has_selection());
    connect(act_copy, &QAction::triggered, this, [this]() { (void)copy_selection(false); });

    QAction* act_copy_hdr = menu.addAction(QStringLiteral("Copy with headers"));
    act_copy_hdr->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    act_copy_hdr->setEnabled(has_selection());
    connect(act_copy_hdr, &QAction::triggered, this, [this]() { (void)copy_selection(true); });

    QAction* act_sel_all = menu.addAction(QStringLiteral("Select All"));
    act_sel_all->setShortcut(QKeySequence(QStringLiteral("Ctrl+A")));
    act_sel_all->setEnabled(storage_ != nullptr && total_rows() > 0);
    connect(act_sel_all, &QAction::triggered, this, &ResultGridViewport::select_all);

    menu.addSeparator();
    QAction* act_save = menu.addAction(QStringLiteral("Save Results As…"));
    act_save->setEnabled(storage_ != nullptr && total_rows() > 0);
    connect(act_save, &QAction::triggered, this, &ResultGridViewport::save_results_requested);

    menu.exec(global_pos);
}

void ResultGridViewport::contextMenuEvent(QContextMenuEvent* event) {
    show_context_menu(event->globalPos());
}

void ResultGridViewport::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    update_scroll_bars();
}

} // namespace capybaradb::ui
