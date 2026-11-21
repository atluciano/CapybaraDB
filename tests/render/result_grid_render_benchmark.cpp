#include "core/result/result_set_storage.hpp"
#include "ui/grid/glyph_atlas.hpp"
#include "ui/grid/grid_frame_builder.hpp"
#include "ui/grid/grid_view_state.hpp"
#include "ui/grid/software_grid_backend.hpp"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

struct BenchStats {
    double mean_ms = 0.0;
    double p50_ms  = 0.0;
    double p95_ms  = 0.0;
    double p99_ms  = 0.0;
    double max_ms  = 0.0;
};

BenchStats compute_stats(std::vector<double> samples) {
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    const auto pick = [&](double q) {
        const size_t idx = std::min<size_t>(
            samples.size() - 1,
            static_cast<size_t>(q * static_cast<double>(samples.size() - 1)));
        return samples[idx];
    };
    return BenchStats{
        .mean_ms = sum / static_cast<double>(samples.size()),
        .p50_ms  = pick(0.50),
        .p95_ms  = pick(0.95),
        .p99_ms  = pick(0.99),
        .max_ms  = samples.back(),
    };
}

void run_resolution_benchmark(
    const char* label,
    const capybaradb::core::ResultSetStorage& rs,
    uint32_t total_rows,
    uint32_t total_cols,
    int phys_w,
    int phys_h,
    float dpr) {
    using namespace capybaradb::ui::grid;

    const int dip_w = static_cast<int>(std::round(static_cast<float>(phys_w) / dpr));
    const int dip_h = static_cast<int>(std::round(static_cast<float>(phys_h) / dpr));

    GridViewState vs;
    vs.columns.init_from_storage(rs);
    for (uint32_t c = 0; c < total_cols; ++c) {
        vs.columns.set_column_width(c, 92.0f, true);
    }
    vs.selection.select_range(10, 2, 25, 8, false);

    GlyphAtlas atlas;
    atlas.ensure_initialized(dpr);
    GridFrameBuilder builder;
    SoftwareGridBackend sw;

    GridFrameInput fin;
    fin.storage              = &rs;
    fin.view_state           = &vs;
    fin.viewport_w_dip       = dip_w;
    fin.viewport_h_dip       = dip_h;
    fin.dpr                  = dpr;
    fin.row_header_w_dip     = compute_row_header_width_dip(total_rows);
    fin.display_json_as_link = true;
    fin.draw_outer_border    = true;

    // Warmup
    for (int i = 0; i < 10; ++i) {
        vs.vscroll_row = (i * 997) % (total_rows - 60);
        vs.hscroll_px  = static_cast<double>((i * 521) % 24000);
        builder.build(fin, atlas);
        sw.render(phys_w, phys_h, builder.rects(), builder.glyphs(), atlas);
    }

    static constexpr int kFrames = 200;
    std::vector<double> build_samples;
    std::vector<double> render_samples;
    std::vector<double> total_samples;
    build_samples.reserve(kFrames);
    render_samples.reserve(kFrames);
    total_samples.reserve(kFrames);

    for (int f = 0; f < kFrames; ++f) {
        vs.vscroll_row = static_cast<int64_t>((f * 1543) % (total_rows - 80));
        vs.hscroll_px  = static_cast<double>((f * 613) % 24000);

        const auto t0 = std::chrono::steady_clock::now();
        builder.build(fin, atlas);
        const auto t1 = std::chrono::steady_clock::now();
        sw.render(phys_w, phys_h, builder.rects(), builder.glyphs(), atlas);
        const auto t2 = std::chrono::steady_clock::now();

        build_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        render_samples.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        total_samples.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
    }

    const BenchStats b = compute_stats(std::move(build_samples));
    const BenchStats r = compute_stats(std::move(render_samples));
    const BenchStats t = compute_stats(std::move(total_samples));

    std::printf("\n=== %s (%dx%d phys, DPR %.2f, rects=%zu, glyphs=%zu) ===\n",
                label, phys_w, phys_h, dpr, builder.rects().size(), builder.glyphs().size());
    std::printf("  build()         : mean=%6.3f ms | p50=%6.3f ms | p95=%6.3f ms | p99=%6.3f ms | max=%6.3f ms\n",
                b.mean_ms, b.p50_ms, b.p95_ms, b.p99_ms, b.max_ms);
    std::printf("  render()        : mean=%6.3f ms | p50=%6.3f ms | p95=%6.3f ms | p99=%6.3f ms | max=%6.3f ms\n",
                r.mean_ms, r.p50_ms, r.p95_ms, r.p99_ms, r.max_ms);
    std::printf("  build()+render(): mean=%6.3f ms | p50=%6.3f ms | p95=%6.3f ms | p99=%6.3f ms | max=%6.3f ms\n",
                t.mean_ms, t.p50_ms, t.p95_ms, t.p99_ms, t.max_ms);
}

} // namespace

int main() {
    using namespace capybaradb::core;
    using namespace capybaradb::ui::grid;

    static constexpr uint32_t kRows = 100000u;
    static constexpr uint32_t kCols = 300u;

    std::printf("Populating %u rows x %u columns (30,000,000 cells)...\n", kRows, kCols);
    auto qres = std::make_unique<QueryExecutionResult>();
    std::vector<std::string> col_names;
    col_names.reserve(kCols);
    std::vector<ColumnSpec> specs;
    specs.reserve(kCols);
    for (uint32_t c = 0; c < kCols; ++c) {
        col_names.push_back("column_header_" + std::to_string(c));
        specs.push_back(ColumnSpec{
            .name          = col_names.back(),
            .type_name     = (c % 3 == 0) ? "int4" : "text",
            .logical_type  = (c % 3 == 0) ? LogicalTypeKind::Int : LogicalTypeKind::Text,
            .may_hold_json = false,
        });
    }

    ResultSetStorage* rs = qres->create_result_set(specs, 256u);
    {
        ResultSetWriter writer(*qres, *rs, 1024ull * 1024 * 1024, 65536u);
        char cell_buf[32]{};
        for (uint32_t r = 0; r < kRows; ++r) {
            (void)writer.begin_row();
            for (uint32_t c = 0; c < kCols; ++c) {
                if (((r + c) & 31u) == 0u) {
                    writer.write_null(c);
                } else {
                    const int n = std::snprintf(cell_buf, sizeof(cell_buf), "R%06u_C%03u", r, c);
                    writer.write_utf8(c, cell_buf, static_cast<size_t>(n));
                }
            }
            (void)writer.end_row(false);
        }
        writer.finish();
    }

    const double used_mib     = static_cast<double>(qres->arena.bytes_used()) / (1024.0 * 1024.0);
    const double reserved_mib = static_cast<double>(qres->arena.bytes_reserved()) / (1024.0 * 1024.0);
    std::printf("Dataset ready: %u rows x %u cols | Arena used=%.2f MiB, reserved=%.2f MiB\n",
                kRows, kCols, used_mib, reserved_mib);

    run_resolution_benchmark("1080p (1920x1080 @ DPR 1.0)", *rs, kRows, kCols, 1920, 1080, 1.0f);
    run_resolution_benchmark("4K (3840x2160 @ DPR 1.5)",    *rs, kRows, kCols, 3840, 2160, 1.5f);
    run_resolution_benchmark("4K (3840x2160 @ DPR 2.0)",    *rs, kRows, kCols, 3840, 2160, 2.0f);

    // Also run nanobench steady-state microbenchmark on 1080p scroll
    GridViewState vs;
    vs.columns.init_from_storage(*rs);
    for (uint32_t c = 0; c < kCols; ++c) {
        vs.columns.set_column_width(c, 92.0f, true);
    }
    GlyphAtlas atlas;
    atlas.ensure_initialized(1.0f);
    GridFrameBuilder builder;
    SoftwareGridBackend sw;
    GridFrameInput fin;
    fin.storage              = rs;
    fin.view_state           = &vs;
    fin.viewport_w_dip       = 1920;
    fin.viewport_h_dip       = 1080;
    fin.dpr                  = 1.0f;
    fin.row_header_w_dip     = compute_row_header_width_dip(kRows);
    fin.display_json_as_link = true;
    fin.draw_outer_border    = true;

    uint64_t step = 0;
    ankerl::nanobench::Bench().minEpochIterations(50).run("1080p build+render (300x100k)", [&]() {
        ++step;
        vs.vscroll_row = static_cast<int64_t>((step * 1543) % (kRows - 80));
        vs.hscroll_px  = static_cast<double>((step * 613) % 24000);
        builder.build(fin, atlas);
        sw.render(1920, 1080, builder.rects(), builder.glyphs(), atlas);
        ankerl::nanobench::doNotOptimizeAway(sw.image().bits());
    });

    return 0;
}
