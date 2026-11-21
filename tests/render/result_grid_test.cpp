#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/stream_sink.hpp"
#include "drivers/common/db_driver.hpp"
#include "ui/editor/query_tab_widget.hpp"
#include "ui/editor/sql_code_editor.hpp"
#include "ui/editor/sql_syntax_highlighter.hpp"
#include "ui/grid/grid_frame_builder.hpp"
#include "ui/grid/result_grid_viewport.hpp"
#include "ui/grid/software_grid_backend.hpp"
#include "ui/shell/main_window.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QTabBar>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// Thread-local allocation hook to prove 0 heap allocations during steady-state grid frames.
namespace {
thread_local bool     g_track_allocs = false;
thread_local uint64_t g_alloc_count  = 0;
} // namespace

void* operator new(std::size_t sz) {
    if (g_track_allocs) {
        ++g_alloc_count;
    }
    if (sz == 0) {
        sz = 1;
    }
    void* ptr = std::malloc(sz);
    if (!ptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

namespace {

bool image_contains_color(const QImage& img, const QColor& expected, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(img.width(), x1);
    y1 = std::min(img.height(), y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (img.pixelColor(x, y) == expected) {
                return true;
            }
        }
    }
    return false;
}

class MockMultiResultDriver final : public capybaradb::drivers::DbDriver {
public:
    std::expected<void, capybaradb::drivers::DbError> connect(
        const capybaradb::drivers::ConnectionConfig& cfg,
        capybaradb::core::CancelToken&) override {
        cfg_ = cfg;
        connected_ = true;
        return {};
    }
    void disconnect() noexcept override { connected_ = false; }
    bool is_connected() const noexcept override { return connected_; }
    capybaradb::drivers::SessionInfo session_info() const override {
        return capybaradb::drivers::SessionInfo{
            .engine         = cfg_.engine,
            .host           = cfg_.host,
            .port           = cfg_.port,
            .server_version = "PostgreSQL 17.2",
            .database       = cfg_.database,
            .username       = cfg_.username,
            .session_id     = 7001,
        };
    }
    std::expected<capybaradb::drivers::ConnectionTestResult, capybaradb::drivers::DbError> test_connection(
        const capybaradb::drivers::ConnectionConfig&,
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::ConnectionTestResult{};
    }
    std::expected<void, capybaradb::drivers::DbError> switch_database(
        std::string_view db,
        capybaradb::core::CancelToken&) override {
        cfg_.database = std::string(db);
        return {};
    }
    std::expected<capybaradb::drivers::SessionLossRisk, capybaradb::drivers::DbError> inspect_loss_risk(
        capybaradb::core::CancelToken&) override {
        return capybaradb::drivers::SessionLossRisk{};
    }
    std::expected<void, capybaradb::drivers::DbError> commit_transaction(
        capybaradb::core::CancelToken&) override {
        return {};
    }

    std::expected<void, capybaradb::drivers::DbError> execute_stream(
        std::string_view sql,
        const capybaradb::drivers::ExecOptions& opts,
        capybaradb::core::QueryExecutionResult& out,
        capybaradb::core::StreamPublishSink& sink,
        capybaradb::core::CancelToken&) override {
        // Result Set 1: id (Int), payload (Json), note (Text with NULL)
        const std::array<capybaradb::core::ColumnSpec, 3> cols1 = {{
            {"id", "int4", capybaradb::core::LogicalTypeKind::Int, false},
            {"payload", "jsonb", capybaradb::core::LogicalTypeKind::Json, true},
            {"note", "text", capybaradb::core::LogicalTypeKind::Text, true},
        }};
        auto* rs1 = out.create_result_set(cols1);
        if (rs1 != nullptr) {
            capybaradb::core::ResultSetWriter w(out, *rs1, opts.max_arena_bytes, opts.max_cell_bytes);
            const std::array<std::tuple<const char*, const char*, const char*>, 3> rows = {{
                {"10", "{\"user\":\"alice\",\"id\":10}", "alpha"},
                {"2",  "{\"user\":\"bob\",\"id\":2}",   nullptr},
                {"30", "not-json",                       "gamma"},
            }};
            for (const auto& [a, b, c] : rows) {
                (void)w.begin_row();
                w.write_utf8(0, a, std::strlen(a));
                w.write_utf8(1, b, std::strlen(b));
                if (c != nullptr) w.write_utf8(2, c, std::strlen(c)); else w.write_null(2);
                (void)w.end_row(false);
            }
            w.finish();
        }

        if (sql.find("TWO_SETS") != std::string_view::npos) {
            const std::array<capybaradb::core::ColumnSpec, 2> cols2 = {{
                {"code", "text", capybaradb::core::LogicalTypeKind::Text, true},
                {"amount", "numeric", capybaradb::core::LogicalTypeKind::Numeric, false},
            }};
            auto* rs2 = out.create_result_set(cols2);
            if (rs2 != nullptr) {
                capybaradb::core::ResultSetWriter w(out, *rs2, opts.max_arena_bytes, opts.max_cell_bytes);
                (void)w.begin_row();
                w.write_utf8(0, "BR", 2);
                w.write_utf8(1, "199.90", 6);
                (void)w.end_row(false);
                (void)w.begin_row();
                w.write_utf8(0, "US", 2);
                w.write_utf8(1, "42.00", 5);
                (void)w.end_row(false);
                w.finish();
            }
        }
        sink.notify_if_unscheduled();
        return {};
    }

    std::expected<void, capybaradb::drivers::DbError> parse_only(
        std::string_view,
        capybaradb::core::CancelToken&) override {
        return {};
    }
    void cancel_active() noexcept override {}

private:
    capybaradb::drivers::ConnectionConfig cfg_;
    bool connected_ = false;
};

} // namespace

class ResultGridTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(capybaradb::ui::theme::apply_classic_blue_theme(*qApp));
    }

    void software_renderer_matches_exact_theme_colors_and_19px_row_metrics() {
        using namespace capybaradb::core;
        using namespace capybaradb::ui;

        QueryExecutionResult qres;
        const std::array<ColumnSpec, 3> cols = {{
            {"id", "int4", LogicalTypeKind::Int, false},
            {"payload", "jsonb", LogicalTypeKind::Json, true},
            {"status", "text", LogicalTypeKind::Text, true},
        }};
        ResultSetStorage* rs = qres.create_result_set(cols);
        QVERIFY(rs != nullptr);
        {
            ResultSetWriter w(qres, *rs, 64ull * 1024 * 1024, 65536u);
            // Row 0: selected cell (0,0), JSON link at (0,1), normal at (0,2)
            QVERIFY(w.begin_row());
            w.write_utf8(0, "101", 3);
            w.write_utf8(1, "{\"ok\":true}", 11);
            w.write_utf8(2, "active", 6);
            (void)w.end_row(false);

            // Row 1: normal (1,0), NULL at (1,1), normal (1,2)
            QVERIFY(w.begin_row());
            w.write_utf8(0, "102", 3);
            w.write_null(1);
            w.write_utf8(2, "idle", 4);
            (void)w.end_row(false);
            w.finish();
        }

        grid::GridViewState vs;
        vs.columns.init_from_storage(*rs);
        vs.columns.set_column_width(0, 80.0f, true);
        vs.columns.set_column_width(1, 140.0f, true);
        vs.columns.set_column_width(2, 100.0f, true);
        vs.columns.set_sort(0, grid::SortDirection::Ascending);
        vs.selection.select_single_cell(0, 0); // Selected cell at (0,0) + dotted focus rect

        ResultGridViewport viewport;
        viewport.set_active_result_set(rs, &vs);
        const QImage img = viewport.render_software_frame(500, 160, 1.0f, true);
        QCOMPARE(img.width(), 500);
        QCOMPARE(img.height(), 160);

        // 1. Outer 1px border (#ABADB3)
        QCOMPARE(img.pixelColor(0, 0), QColor(0xAB, 0xAD, 0xB3));
        QCOMPARE(img.pixelColor(499, 159), QColor(0xAB, 0xAD, 0xB3));

        // 2. Top-Left Select-All corner (#E1E1E1) & its 1px dividers (#D0D0D0)
        // Origin shifted by 1px due to outer border; row_header_w = 32, col_header_h = 19
        QCOMPARE(img.pixelColor(5, 5), QColor(0xE1, 0xE1, 0xE1));
        QCOMPARE(img.pixelColor(32, 5), QColor(0xD0, 0xD0, 0xD0));  // right divider at 1 + 32 - 1 = 32
        QCOMPARE(img.pixelColor(5, 19), QColor(0xD0, 0xD0, 0xD0));  // bottom divider at 1 + 19 - 1 = 19

        // 3. Column Header (#F0F0F0) height = 19 px + right divider (#D0D0D0) + Sort Arrow (#717171)
        QCOMPARE(img.pixelColor(35, 3), QColor(0xF0, 0xF0, 0xF0));
        QCOMPARE(img.pixelColor(33 + 80 - 1, 5), QColor(0xD0, 0xD0, 0xD0));
        QVERIFY(image_contains_color(img, QColor(0x71, 0x71, 0x71), 33 + 60, 1, 33 + 79, 19));

        // 4. Row Header (#F0F0F0) row 0 [y=20..38] + row divider at y=38 (#D0D0D0) -> exact 19px height!
        QCOMPARE(img.pixelColor(3, 22), QColor(0xF0, 0xF0, 0xF0));
        QCOMPARE(img.pixelColor(5, 38), QColor(0xD0, 0xD0, 0xD0));  // 20 + 19 - 1 = 38
        QCOMPARE(img.pixelColor(5, 57), QColor(0xD0, 0xD0, 0xD0));  // 39 + 19 - 1 = 57

        // 5. Selected Cell (0,0) background (#A5CFF1) & dotted focus rect (#000000)
        QCOMPARE(img.pixelColor(35, 22), QColor(0xA5, 0xCF, 0xF1));
        QVERIFY(image_contains_color(img, QColor(0x00, 0x00, 0x00), 33, 20, 33 + 80, 39));

        // 6. NULL Cell (1,1) background (#FDFBAC)
        QCOMPARE(img.pixelColor(33 + 80 + 3, 39 + 2), QColor(0xFD, 0xFB, 0xAC));

        // 7. Grid Lines (#CCCEDB) at row bottom (y=38) and col right (x=112), stopping after last column (x=353)
        QCOMPARE(img.pixelColor(33 + 5, 38), QColor(0xCC, 0xCE, 0xDB));
        QCOMPARE(img.pixelColor(33 + 80 - 1, 25), QColor(0xCC, 0xCE, 0xDB));
        // Empty area right of last column & below last row must be #F5F5F5 (no stray grid lines)
        QCOMPARE(img.pixelColor(450, 25), QColor(0xF5, 0xF5, 0xF5));
        QCOMPARE(img.pixelColor(450, 38), QColor(0xF5, 0xF5, 0xF5));
        QCOMPARE(img.pixelColor(60, 120), QColor(0xF5, 0xF5, 0xF5));

        // 8. JSON link color (#0066CC) & underline in cell (0,1)
        QVERIFY(image_contains_color(img, QColor(0x00, 0x66, 0xCC), 33 + 80, 20, 33 + 80 + 140, 39));
    }

    void query_tab_manages_multiple_result_sets_non_mutating_sort_clipboard_copy_and_json_preview_tabs() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        capybaradb::ui::MainWindow win(tmp.path().toStdWString());
        win.resize(1200, 750);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        QVERIFY(tab != nullptr);
        tab->set_driver_factory([](capybaradb::drivers::EngineKind) {
            return std::make_unique<MockMultiResultDriver>();
        });

        capybaradb::drivers::ConnectionConfig cfg;
        cfg.engine   = capybaradb::drivers::EngineKind::PostgreSql;
        cfg.host     = "127.0.0.1";
        cfg.port     = 5432;
        cfg.database = "appdb";
        cfg.username = "postgres";

        tab->connect_async(cfg, &win.worker_pool());
        QTRY_VERIFY(tab->is_connected());

        // Single resultset -> ResultSetsStrip stays hidden
        tab->set_results_destination(capybaradb::core::ResultsDestination::Grid);
        tab->set_editor_text(QStringLiteral("SELECT 1;"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QVERIFY(!tab->result_sets_strip()->isVisible());
        QVERIFY(tab->result_grid()->storage() != nullptr);
        QCOMPARE(tab->result_grid()->storage()->published_rows.load(), 3ull);

        // Track lifetime of the first QueryExecutionResult to prove zero shared_ptr cycle leak on re-execution!
        std::weak_ptr<capybaradb::core::QueryExecutionResult> first_result_weak = tab->current_result();
        QVERIFY(!first_result_weak.expired());

        // Rule §3.2: arrival of 1st row sets CurrentCell=(0,0) WITHOUT selecting
        QVERIFY(tab->result_grid()->view_state()->selection.current_cell().has_value());
        QCOMPARE(tab->result_grid()->view_state()->selection.current_cell()->row, 0);
        QCOMPARE(tab->result_grid()->view_state()->selection.current_cell()->col_ordinal, 0);
        QVERIFY(!tab->result_grid()->has_selection());

        // Two resultsets -> ResultSetsStrip becomes visible with Result 1 / Result 2
        // AND the previous QueryExecutionResult (and its entire ArenaAllocator) MUST be destroyed immediately!
        tab->set_editor_text(QStringLiteral("TWO_SETS"));
        tab->execute_query();
        QTRY_VERIFY(!tab->is_busy());
        QTRY_VERIFY(first_result_weak.expired());
        QVERIFY(tab->result_sets_strip()->isVisible());
        QCOMPARE(tab->result_sets_strip()->count(), 2);
        QCOMPARE(tab->result_sets_strip()->tabText(0), QStringLiteral("Result 1"));
        QCOMPARE(tab->result_sets_strip()->tabText(1), QStringLiteral("Result 2"));

        // Local sort ASC on Result 1 col 0 ("id": 10, 2, 30 -> 2, 10, 30) WITHOUT mutating Arena
        auto* rs1 = tab->result_grid()->storage();
        auto* vs1 = tab->result_grid()->view_state();
        vs1->toggle_sort(*rs1, 0);
        QCOMPARE(vs1->resolve_physical_row(0), 1ull); // id=2
        QCOMPARE(vs1->resolve_physical_row(1), 0ull); // id=10
        QCOMPARE(vs1->resolve_physical_row(2), 2ull); // id=30
        // Arena physical row 0 is still "10"
        const auto [chunk0, lr0] = rs1->locate_row(0);
        QCOMPARE(chunk0->text_of(chunk0->cell(lr0, 0)), std::string_view{"10"});

        // Select row 0 in Result 1 and switch to Result 2 -> independent state preserved
        vs1->selection.select_single_cell(0, 0);
        tab->result_sets_strip()->setCurrentIndex(1);
        QCOMPARE(tab->result_grid()->storage()->published_rows.load(), 2ull);
        QVERIFY(!tab->result_grid()->has_selection());

        // Switch back to Result 1 -> selection and sort order preserved
        tab->result_sets_strip()->setCurrentIndex(0);
        QVERIFY(tab->result_grid()->has_selection());
        QCOMPARE(tab->result_grid()->view_state()->sort_dir, capybaradb::ui::grid::SortDirection::Ascending);

        // Copy selected cell (view row 0 = physical row 1 = "2")
        QVERIFY(tab->result_grid()->copy_selection(false));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("2"));

        // Copy with headers
        QVERIFY(tab->result_grid()->copy_selection(true));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("id\r\n2"));

        // AutoFit column marks user_resized = true
        tab->result_grid()->auto_fit_column(1);
        QVERIFY(tab->result_grid()->view_state()->columns.column(1).user_resized);

        // Clicking a JSON link emits open_json_document_requested -> opens JSON1.json unconnected tab without SQL highlighting
        emit tab->result_grid()->json_link_clicked(QStringLiteral("{\n  \"id\": 10\n}"));
        QCOMPARE(win.query_tab_count(), 2);
        QCOMPARE(win.active_tab_title(), QStringLiteral("JSON1.json"));
        capybaradb::ui::QueryTabWidget* json_tab = win.active_query();
        QVERIFY(json_tab != nullptr);
        QVERIFY(!json_tab->is_connected());
        QVERIFY(!json_tab->editor_widget()->syntax_highlighter()->is_highlighting_enabled());
        QVERIFY(json_tab->editor_text().contains(QStringLiteral("\"id\": 10")));
    }

    void grid_frame_builder_renders_15m_cells_at_60fps_with_zero_steady_state_heap_allocations() {
        using namespace capybaradb::core;
        using namespace capybaradb::ui::grid;

        static constexpr uint32_t kRows = 50000u;
        static constexpr uint32_t kCols = 300u;

        auto qres = std::make_unique<QueryExecutionResult>();
        std::vector<std::string> col_names;
        col_names.reserve(kCols);
        std::vector<ColumnSpec> specs;
        specs.reserve(kCols);
        for (uint32_t c = 0; c < kCols; ++c) {
            col_names.push_back("col_" + std::to_string(c));
            specs.push_back(ColumnSpec{
                .name          = col_names.back(),
                .type_name     = (c % 3 == 0) ? "int4" : "text",
                .logical_type  = (c % 3 == 0) ? LogicalTypeKind::Int : LogicalTypeKind::Text,
                .may_hold_json = false,
            });
        }

        ResultSetStorage* rs = qres->create_result_set(specs, 256u);
        QVERIFY(rs != nullptr);

        // Populate 50,000 x 300 = 15,000,000 cells (average ~12 bytes UTF-8 per cell, with periodic NULLs)
        {
            ResultSetWriter writer(*qres, *rs, 512ull * 1024 * 1024, 65536u);
            char cell_buf[32]{};
            for (uint32_t r = 0; r < kRows; ++r) {
                QVERIFY(writer.begin_row());
                for (uint32_t c = 0; c < kCols; ++c) {
                    if (((r + c) & 31u) == 0u) {
                        writer.write_null(c);
                    } else {
                        const int n = std::snprintf(cell_buf, sizeof(cell_buf), "R%05u_C%03u", r, c);
                        writer.write_utf8(c, cell_buf, static_cast<size_t>(n));
                    }
                }
                (void)writer.end_row(false);
            }
            writer.finish();
        }

        QCOMPARE(rs->published_rows.load(std::memory_order_acquire), static_cast<uint64_t>(kRows));
        const double used_mib     = static_cast<double>(qres->arena.bytes_used()) / (1024.0 * 1024.0);
        const double reserved_mib = static_cast<double>(qres->arena.bytes_reserved()) / (1024.0 * 1024.0);
        std::printf("[BENCH 50k x 300] Arena used=%.2f MiB, reserved=%.2f MiB, blocks=%u\n",
                    used_mib, reserved_mib, qres->arena.block_count());
        QVERIFY2(reserved_mib < 350.0, "Arena reserved RAM must stay below 350 MiB for 15M cells");

        // Prepare view state, GlyphAtlas, GridFrameBuilder, and SoftwareGridBackend
        GridViewState vs;
        vs.columns.init_from_storage(*rs);
        for (uint32_t c = 0; c < kCols; ++c) {
            vs.columns.set_column_width(c, 92.0f, true);
        }
        vs.selection.select_all(kRows, static_cast<int32_t>(kCols));

        GlyphAtlas atlas;
        atlas.ensure_initialized(1.0f);
        GridFrameBuilder builder;
        SoftwareGridBackend sw;

        GridFrameInput fin;
        fin.storage              = rs;
        fin.view_state           = &vs;
        fin.viewport_w_dip       = 1440;
        fin.viewport_h_dip       = 600;
        fin.dpr                  = 1.0f;
        fin.row_header_w_dip     = compute_row_header_width_dip(kRows);
        fin.display_json_as_link = true;
        fin.draw_outer_border    = true;

        // Warm-up frame (ensures builder vectors and QImage are at peak capacity)
        builder.build(fin, atlas);
        sw.render(1440, 600, builder.rects(), builder.glyphs(), atlas);

        // Sweep 60 frames across vertical and horizontal scroll positions, proving:
        // 1) ZERO heap allocations in GridFrameBuilder::build()
        // 2) Frame time < 16 ms (>= 60 FPS) even including CPU rasterization!
        static constexpr int kFrames = 60;
        uint64_t total_build_allocs = 0;
        double build_ms_total = 0.0;
        double full_ms_total  = 0.0;

        for (int f = 0; f < kFrames; ++f) {
            vs.vscroll_row = static_cast<int64_t>((f * 811) % (kRows - 40));
            vs.hscroll_px  = static_cast<double>((f * 437) % 25000);

            const auto t0 = std::chrono::steady_clock::now();
            g_alloc_count  = 0;
            g_track_allocs = true;
            builder.build(fin, atlas);
            g_track_allocs = false;
            total_build_allocs += g_alloc_count;
            const auto t1 = std::chrono::steady_clock::now();

            sw.render(1440, 600, builder.rects(), builder.glyphs(), atlas);
            const auto t2 = std::chrono::steady_clock::now();

            build_ms_total += std::chrono::duration<double, std::milli>(t1 - t0).count();
            full_ms_total  += std::chrono::duration<double, std::milli>(t2 - t0).count();
        }

        const double avg_build_ms = build_ms_total / kFrames;
        const double avg_full_ms  = full_ms_total / kFrames;
        const double fps_sw       = (avg_full_ms > 0.0) ? (1000.0 / avg_full_ms) : 9999.0;
        std::printf("[BENCH 50k x 300] avg build=%.3f ms, avg sw_frame=%.3f ms (%.1f FPS), build_allocs=%llu\n",
                    avg_build_ms, avg_full_ms, fps_sw, static_cast<unsigned long long>(total_build_allocs));

        QCOMPARE(total_build_allocs, 0ull);
        QVERIFY2(avg_build_ms < 4.0, "GridFrameBuilder::build must take < 4 ms/frame");
        QVERIFY2(avg_full_ms < 16.0, "Full frame render (even on Software backend) must stay < 16 ms (>= 60 FPS)");

        // Measure Arena OS release (< 2 ms required by ROADMAP Milestone 4 exit criterion)
        const auto r0 = std::chrono::steady_clock::now();
        qres->arena.reset();
        const double release_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
        std::printf("[BENCH 50k x 300] Arena release=%.3f ms\n", release_ms);
        QVERIFY2(release_ms < 2.0, "Arena OS release must complete in < 2 ms");
    }

    void glyph_atlas_cache_shares_instances_per_dpr_and_grows_dynamically_from_512_to_2048() {
        using namespace capybaradb::ui;
        using namespace capybaradb::ui::grid;

        clear_glyph_atlas_cache();

        // 1. Multiple ResultGridViewports at the same DPR share the exact same GlyphAtlas instance
        ResultGridViewport vp1;
        ResultGridViewport vp2;
        ResultGridViewport vp3;
        QCOMPARE(&vp1.glyph_atlas(), &vp2.glyph_atlas());
        QCOMPARE(&vp2.glyph_atlas(), &vp3.glyph_atlas());

        // Initial allocation starts at 512x512 (256 KiB), not 2048x2048 (4 MiB)
        QCOMPARE(vp1.glyph_atlas().width(), 512u);
        QCOMPARE(vp1.glyph_atlas().height(), 512u);
        QCOMPARE(vp1.glyph_atlas().byte_size(), static_cast<size_t>(512u * 512u));

        // 2. Multiple DPRs (1.0, 1.25, 1.5, 2.0) get distinct cached atlases, each starting at 512x512
        const std::array<float, 4> dprs = {1.0f, 1.25f, 1.5f, 2.0f};
        for (float dpr : dprs) {
            GlyphAtlas* a = glyph_atlas_for_dpr(dpr);
            GlyphAtlas* b = glyph_atlas_for_dpr(dpr);
            QVERIFY(a != nullptr);
            QCOMPARE(a, b);
            QCOMPARE(a->width(), 512u);
            QCOMPARE(a->height(), 512u);
            QVERIFY(std::abs(a->dpr() - dpr) < 0.01f);
        }
        QCOMPARE(cached_glyph_atlas_dpr_count(), static_cast<size_t>(4));

        // 3. Dynamic shelf growth: 512x512 -> 1024x1024 -> 2048x2048 preserves existing glyph pixels & integer coordinates
        GlyphAtlas grow_test;
        grow_test.ensure_initialized(1.0f);
        QCOMPARE(grow_test.width(), 512u);

        const GlyphMetric before_A = grow_test.latin1('A');
        QVERIFY(before_A.valid);
        const uint16_t orig_ax = before_A.sx;
        const uint16_t orig_ay = before_A.sy;
        const uint8_t sample_px = (before_A.w > 0.0f && before_A.h > 0.0f)
            ? grow_test.pixels()[static_cast<size_t>(orig_ay + static_cast<uint16_t>(before_A.h) / 2u) * grow_test.width() + (orig_ax + static_cast<uint16_t>(before_A.w) / 2u)]
            : 0u;

        // Pack 128x128 blocks until 512x512 overflows into 1024x1024
        GlyphMetric custom_m{};
        for (int i = 0; i < 16; ++i) {
            QVERIFY(grow_test.pack_custom_rect_for_test(128u, 128u, static_cast<uint8_t>(0xA0 + i), custom_m));
        }
        QCOMPARE(grow_test.width(), 1024u);
        QCOMPARE(grow_test.height(), 1024u);
        QCOMPARE(grow_test.byte_size(), static_cast<size_t>(1024u * 1024u));

        // Verify 'A' integer coordinates (sx, sy) and bitmap pixel at (orig_ax, orig_ay) are preserved!
        const GlyphMetric after_1024_A = grow_test.latin1('A');
        QCOMPARE(after_1024_A.sx, orig_ax);
        QCOMPARE(after_1024_A.sy, orig_ay);
        if (before_A.w > 0.0f && before_A.h > 0.0f) {
            const uint8_t px_1024 = grow_test.pixels()[static_cast<size_t>(orig_ay + static_cast<uint16_t>(before_A.h) / 2u) * grow_test.width() + (orig_ax + static_cast<uint16_t>(before_A.w) / 2u)];
            QCOMPARE(px_1024, sample_px);
        }

        // Pack 256x256 blocks until 1024x1024 overflows into 2048x2048
        for (int i = 0; i < 16; ++i) {
            QVERIFY(grow_test.pack_custom_rect_for_test(256u, 256u, static_cast<uint8_t>(0xB0 + i), custom_m));
        }
        QCOMPARE(grow_test.width(), 2048u);
        QCOMPARE(grow_test.height(), 2048u);
        QCOMPARE(grow_test.byte_size(), static_cast<size_t>(2048u * 2048u));
        QCOMPARE(grow_test.latin1('A').sx, orig_ax);
        QCOMPARE(grow_test.latin1('A').sy, orig_ay);
    }

    void main_window_preserves_native_window_handle_when_opening_and_closing_query_tabs() {
        if (QGuiApplication::platformName().compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0) {
            QSKIP("Native HWND recreation check only applies to live desktop QPA platforms.");
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        capybaradb::ui::MainWindow win(tmp.path().toStdWString());
        win.resize(1000, 650);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        struct WinIdWatcher final : public QObject {
            int changes = 0;
            bool eventFilter(QObject*, QEvent* e) override {
                if (e->type() == QEvent::WinIdChange) {
                    ++changes;
                }
                return false;
            }
        } watcher;
        win.installEventFilter(&watcher);

        const WId id_before = win.winId();
        capybaradb::ui::QueryTabWidget* tab = win.new_query();
        QVERIFY(tab != nullptr);
        QTest::qWait(100);

        QCOMPARE(watcher.changes, 0);
        QCOMPARE(win.winId(), id_before);

        // Closing all tabs and opening a second tab must also never recreate HWND
        QVERIFY(win.close_active_query());
        QTest::qWait(50);
        capybaradb::ui::QueryTabWidget* tab2 = win.new_query();
        QVERIFY(tab2 != nullptr);
        QTest::qWait(100);

        QCOMPARE(watcher.changes, 0);
        QCOMPARE(win.winId(), id_before);
    }
};

QTEST_MAIN(ResultGridTest)
#include "result_grid_test.moc"
