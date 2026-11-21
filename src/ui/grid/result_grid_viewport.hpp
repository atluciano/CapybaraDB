#pragma once

#include "core/result/result_set_storage.hpp"
#include "ui/grid/glyph_atlas.hpp"
#include "ui/grid/grid_frame_builder.hpp"
#include "ui/grid/grid_view_state.hpp"
#include "ui/grid/software_grid_backend.hpp"

#include <QFrame>
#include <QImage>
#include <QPointF>
#include <QString>
#include <cstdint>

class QGridLayout;
class QKeyEvent;
class QMouseEvent;
class QScrollBar;
class QTimer;
class QWheelEvent;

namespace capybaradb::ui {

namespace detail {
class GridSurfaceWidget;
} // namespace detail

/// High-performance virtualized result grid widget.
/// Renders directly via GridFrameBuilder + SoftwareGridBackend (CPU QImage rasterizer)
/// with zero heap allocations per frame. Switching ResultSets swaps (ResultSetStorage*, GridViewState*) in O(1).
class ResultGridViewport final : public QFrame {
    Q_OBJECT

public:
    explicit ResultGridViewport(QWidget* parent = nullptr);
    ~ResultGridViewport() override;

    /// Switches the displayed ResultSet and its preserved view state in O(1).
    void set_active_result_set(
        const core::ResultSetStorage* storage,
        grid::GridViewState* view_state);

    /// Called when new rows have been published to the active ResultSetStorage.
    void notify_rows_available();

    void clear();

    [[nodiscard]] const core::ResultSetStorage* storage() const noexcept { return storage_; }
    [[nodiscard]] grid::GridViewState* view_state() const noexcept { return view_state_; }

    [[nodiscard]] bool display_json_as_link() const noexcept { return display_json_as_link_; }
    void set_display_json_as_link(bool enabled);

    [[nodiscard]] bool retain_line_breaks_on_copy() const noexcept { return retain_line_breaks_; }
    void set_retain_line_breaks_on_copy(bool retain) noexcept { retain_line_breaks_ = retain; }

    [[nodiscard]] bool has_selection() const noexcept;
    [[nodiscard]] float row_header_width_dip() const noexcept { return row_header_w_dip_; }

    /// Copies current selection as TSV to the OS clipboard (2-pass 4 MiB / 250k cell safe guard).
    /// Returns true on success; emits status_message on error or completion.
    bool copy_selection(bool include_headers);
    void select_all();
    void auto_fit_column(int32_t visible_col_idx);

    /// Deterministic software raster render for headless pixel-parity testing.
    [[nodiscard]] QImage render_software_frame(int width_dip, int height_dip, float dpr = 1.0f, bool draw_outer_border = true);

    [[nodiscard]] grid::GlyphAtlas& glyph_atlas();
    [[nodiscard]] grid::GridFrameBuilder& frame_builder() noexcept { return frame_builder_; }

    [[nodiscard]] QScrollBar* vertical_scroll_bar() const noexcept { return v_scroll_; }
    [[nodiscard]] QScrollBar* horizontal_scroll_bar() const noexcept { return h_scroll_; }

signals:
    void selection_changed();
    void json_link_clicked(const QString& pretty_json);
    void save_results_requested();
    void status_message(const QString& message);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    friend class detail::GridSurfaceWidget;

    enum class DragKind : uint8_t {
        None = 0,
        Cells,
        RowHeaders,
        ColumnResize,
        ColumnHeaderClick,
    };

    void create_surface();
    void update_surface();
    void update_scroll_bars();
    void sample_rows_for_auto_size();
    void ensure_cell_visible(int64_t row, int32_t col_ordinal);
    void move_current_cell(int64_t row, int32_t col_ordinal, bool extend_selection);
    void show_context_menu(const QPoint& global_pos);

    void on_surface_paint_software(QPainter& painter, int w_dip, int h_dip, float dpr);
    void on_surface_mouse_press(QMouseEvent* event);
    void on_surface_mouse_move(QMouseEvent* event);
    void on_surface_mouse_release(QMouseEvent* event);
    void on_surface_mouse_double_click(QMouseEvent* event);
    void on_auto_scroll_tick();

    [[nodiscard]] int64_t total_rows() const noexcept;
    [[nodiscard]] int64_t full_visible_rows() const noexcept;
    [[nodiscard]] double  data_viewport_w_dip() const noexcept;
    [[nodiscard]] double  data_viewport_h_dip() const noexcept;
    [[nodiscard]] bool    hit_test_data_cell(const QPointF& pos, int64_t& row_out, int32_t& col_ord_out) const;

    const core::ResultSetStorage* storage_    = nullptr;
    grid::GridViewState*          view_state_ = nullptr;

    [[nodiscard]] grid::GlyphAtlas& ensure_atlas(float dpr);

    grid::GlyphAtlas*          atlas_ = nullptr;
    grid::GridFrameBuilder     frame_builder_;
    grid::SoftwareGridBackend  sw_backend_;

    QGridLayout* grid_layout_       = nullptr;
    QWidget*     surface_           = nullptr;
    QScrollBar*  v_scroll_          = nullptr;
    QScrollBar*  h_scroll_          = nullptr;
    QWidget*     corner_filler_     = nullptr;
    QTimer*      auto_scroll_timer_ = nullptr;

    bool   display_json_as_link_ = true;
    bool   retain_line_breaks_   = true;
    bool   updating_scroll_bars_ = false;
    float  row_header_w_dip_     = grid::kMinRowHeaderWidthDip;

    // Mouse drag interaction state
    DragKind active_drag_            = DragKind::None;
    bool     additive_drag_          = false;
    int32_t  resizing_vis_col_       = -1;
    double   resize_start_x_         = 0.0;
    float    resize_start_w_         = 64.0f;
    int32_t  pressed_header_vis_col_ = -1;
    bool     drag_left_initial_cell_ = false;
    bool     has_pressed_cell_       = false;
    grid::GridCellPos pressed_cell_{};
    QPointF  last_mouse_pos_;
};

} // namespace capybaradb::ui
