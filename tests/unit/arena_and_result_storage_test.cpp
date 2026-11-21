#include "core/arena/arena_allocator.hpp"
#include "core/result/cell_desc.hpp"
#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/stream_sink.hpp"
#include "ui/grid/column_metrics.hpp"
#include "ui/grid/grid_selection_model.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("ArenaAllocator maintains alignment, pointer stability across block growth, and O(blocks) OS release", "[core][arena]") {
    capybaradb::core::ArenaAllocator arena;
    REQUIRE(arena.bytes_reserved() == 0);
    REQUIRE(arena.bytes_used() == 0);
    REQUIRE(arena.block_count() == 0);

    // Alignment & pointer stability across block growth
    uint64_t* first = arena.alloc_array<uint64_t>(16);
    REQUIRE(first != nullptr);
    REQUIRE((reinterpret_cast<uintptr_t>(first) % alignof(uint64_t)) == 0);
    for (int i = 0; i < 16; ++i) {
        first[i] = static_cast<uint64_t>(0xCAFE0000u + i);
    }

    // Allocate > 20 MiB to force multiple OS blocks (1 -> 2 -> 4 -> 8 -> 16 MiB)
    for (int b = 0; b < 24; ++b) {
        char* p = arena.alloc_bytes(1024 * 1024);
        REQUIRE(p != nullptr);
        p[0] = static_cast<char>(b);
    }
    REQUIRE(arena.block_count() >= 4u);
    REQUIRE(arena.bytes_used() >= 24ull * 1024 * 1024);

    // Earlier pointer remains 100% intact and unmoved
    for (int i = 0; i < 16; ++i) {
        REQUIRE(first[i] == static_cast<uint64_t>(0xCAFE0000u + i));
    }

    // Move semantics preserve ownership
    capybaradb::core::ArenaAllocator moved(std::move(arena));
    REQUIRE(arena.block_count() == 0);
    REQUIRE(moved.block_count() >= 4u);
    REQUIRE(first[0] == 0xCAFE0000u);

    // O(block_count) release back to OS
    const auto t0 = std::chrono::steady_clock::now();
    moved.reset();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(moved.bytes_reserved() == 0);
    REQUIRE(moved.bytes_used() == 0);
    REQUIRE(moved.block_count() == 0);
    REQUIRE(ms < 50.0);

    // TempArena / ScopedTempArena rewinds bump pointer in O(1) and reuses committed blocks
    std::string_view perm = moved.push_string("permanent_header");
    REQUIRE(perm == "permanent_header");
    const size_t used_after_perm = moved.bytes_used();
    const uint32_t blocks_before = moved.block_count();
    REQUIRE(blocks_before == 1u);

    {
        capybaradb::core::ScopedTempArena outer_scratch(moved);
        auto span = moved.push_span<uint64_t>(1024);
        REQUIRE(span.size() == 1024u);
        span[0] = 0x1122334455667788ull;
        const size_t used_outer = moved.bytes_used();
        REQUIRE(used_outer > used_after_perm);

        // Nested TempArena checkpoint inside outer_scratch
        {
            capybaradb::core::TempArena inner = moved.begin_temp();
            struct SamplePod {
                uint64_t a = 7;
                uint32_t b = 9;
            };
            SamplePod* s = moved.alloc_struct<SamplePod>();
            REQUIRE(s != nullptr);
            REQUIRE(s->a == 0ull);
            REQUIRE(s->b == 0u);
            s->a = 42ull;
            s->b = 99u;
            REQUIRE(s->a == 42ull);
            REQUIRE(s->b == 99u);
            std::string_view inner_str = moved.push_string("scratch_nested_string");
            REQUIRE(inner_str == "scratch_nested_string");
            REQUIRE(moved.bytes_used() > used_outer);
            moved.end_temp(inner);
        }
        REQUIRE(moved.bytes_used() == used_outer);
        REQUIRE(span[0] == 0x1122334455667788ull);
    }
    REQUIRE(moved.bytes_used() == used_after_perm);
    REQUIRE(moved.block_count() == blocks_before);
    REQUIRE(perm == "permanent_header");
}

TEST_CASE("ResultSetStorage and CellDesc locate rows across uniform and non-uniform chunks and enforce arena byte limits", "[core][result]") {
    using namespace capybaradb::core;

    static_assert(sizeof(CellDesc) == 8 && alignof(CellDesc) == 8);
    const CellDesc d_null = CellDesc::make_null();
    REQUIRE(d_null.is_null());
    REQUIRE(d_null.byte_len() == 0);

    const CellDesc d_val = CellDesc::make(128u, 42u, kCellHasNewline | kCellJsonLike);
    REQUIRE_FALSE(d_val.is_null());
    REQUIRE(d_val.offset == 128u);
    REQUIRE(d_val.byte_len() == 42u);
    REQUIRE((d_val.flags() & kCellHasNewline) != 0);
    REQUIRE((d_val.flags() & kCellJsonLike) != 0);

    QueryExecutionResult qres;
    const std::array<ColumnSpec, 4> cols = {{
        {"id", "int4", LogicalTypeKind::Int, false},
        {"payload", "jsonb", LogicalTypeKind::Json, true},
        {"notes", "text", LogicalTypeKind::Text, true},
        {"raw", "bytea", LogicalTypeKind::Binary, false},
    }};
    ResultSetStorage* rs = qres.create_result_set(cols, 16);
    REQUIRE(rs != nullptr);

    // Flush early at row 5 (non-uniform chunks) and then fill to 40 rows
    {
        ResultSetWriter writer(qres, *rs, 64ull * 1024 * 1024, 65536u);
        for (int r = 0; r < 40; ++r) {
            REQUIRE(writer.begin_row());
            const std::string id_s = std::to_string(r);
            writer.write_utf8(0, id_s.data(), id_s.size());

            if (r % 5 == 0) {
                writer.write_null(1);
            } else {
                const std::string js = "{\"k\":" + id_s + "}";
                writer.write_utf8(1, js.data(), js.size());
            }

            if (r == 2) {
                const char16_t u16[] = u"Ol\u00E1\nMundo";
                writer.write_utf16(2, u16, 9);
            } else {
                const std::string note = "line_" + id_s;
                writer.write_utf8(2, note.data(), note.size());
            }

            const std::array<uint8_t, 2> bin = {0xDE, 0xAD};
            writer.write_binary_hex(3, bin.data(), bin.size(), "0x");
            (void)writer.end_row(r == 4); // force early flush at 5 rows -> non-uniform chunks
        }
        writer.finish();
    }

    REQUIRE(rs->complete.load(std::memory_order_acquire));
    REQUIRE(rs->published_rows.load(std::memory_order_acquire) == 40u);
    REQUIRE_FALSE(rs->uniform_chunks.load(std::memory_order_acquire));

    for (uint64_t r = 0; r < 40u; ++r) {
        const auto [chunk, lr] = rs->locate_row(r);
        REQUIRE(chunk != nullptr);
        REQUIRE(chunk->text_of(chunk->cell(lr, 0)) == std::to_string(r));
        if (r % 5 == 0) {
            REQUIRE(chunk->cell(lr, 1).is_null());
            REQUIRE(chunk->text_of(chunk->cell(lr, 1)) == "NULL");
        } else {
            REQUIRE((chunk->cell(lr, 1).flags() & kCellJsonLike) != 0);
        }
        REQUIRE((chunk->cell(lr, 3).flags() & kCellBinary) != 0);
        REQUIRE(chunk->text_of(chunk->cell(lr, 3)) == "0xDEAD");
    }

    // Verify UTF-16LE transcoding + non-ASCII + newline flags on row 2
    {
        const auto [chunk, lr] = rs->locate_row(2);
        REQUIRE(chunk != nullptr);
        const CellDesc& c2 = chunk->cell(lr, 2);
        REQUIRE((c2.flags() & kCellNonAscii) != 0);
        REQUIRE((c2.flags() & kCellHasNewline) != 0);
        REQUIRE(chunk->text_of(c2) == "Ol\xC3\xA1\nMundo");
    }

    // Verify max_arena_bytes enforcement sets kLimitArenaBytes cleanly without OOM
    QueryExecutionResult capped;
    ResultSetStorage* crs = capped.create_result_set(cols, 4);
    REQUIRE(crs != nullptr);
    {
        ResultSetWriter writer(capped, *crs, 4096u, 65536u);
        const std::string big(512, 'X');
        for (int r = 0; r < 50; ++r) {
            if (!writer.begin_row()) break;
            writer.write_utf8(0, "1", 1);
            writer.write_utf8(1, big.data(), big.size());
            writer.write_utf8(2, big.data(), big.size());
            writer.write_utf8(3, big.data(), big.size());
            (void)writer.end_row(false);
        }
        writer.finish();
        REQUIRE(writer.arena_limit_hit());
    }
    REQUIRE((crs->limit_reasons.load(std::memory_order_acquire) & kLimitArenaBytes) != 0);
    REQUIRE(capped.any_limit_hit());
}

TEST_CASE("StreamPublishSink coalesces UI notifications via atomic_flag and triggers on row milestones", "[core][threading][stream]") {
    capybaradb::core::StreamPublishSink sink;
    int callback_count = 0;
    sink.set_callback([&]() { ++callback_count; });

    // Milestone 1 fires once and stays pending until UI clears
    REQUIRE(sink.on_rows_published(1));
    REQUIRE(callback_count == 1);
    REQUIRE(sink.is_pending_for_test());

    // Milestone 30 & 100 are coalesced while pending
    REQUIRE(sink.on_rows_published(30));
    REQUIRE(sink.on_rows_published(100));
    REQUIRE(callback_count == 1);

    // Clear pending -> next milestone (10,000) fires callback again
    sink.clear_pending();
    REQUIRE_FALSE(sink.is_pending_for_test());
    REQUIRE(sink.on_rows_published(10000));
    REQUIRE(callback_count == 2);

    // Inline 64-byte functor storage and raw function pointer overload (zero std::function heap alloc)
    sink.clear_pending();
    std::array<uint64_t, 4> captured = {10, 20, 30, 40};
    uint64_t sum = 0;
    sink.set_callback([captured, &sum]() {
        sum = captured[0] + captured[1] + captured[2] + captured[3];
    });
    sink.notify_if_unscheduled();
    REQUIRE(sum == 100);

    sink.clear_pending();
    int raw_hits = 0;
    sink.set_callback(+[](void* ctx) noexcept {
        *static_cast<int*>(ctx) += 5;
    }, &raw_hits);
    sink.notify_if_unscheduled();
    REQUIRE(raw_hits == 5);
}

TEST_CASE("ColumnMetrics computes visible column ranges in O(log M), hit-tests dividers, and reorders display indices", "[ui][grid]") {
    using namespace capybaradb::ui::grid;

    ColumnMetrics cm;
    for (uint32_t i = 0; i < 300; ++i) {
        GridColumnInfo c;
        c.caption = "col_" + std::to_string(i);
        c.width   = 100.0f;
        cm.add_column(std::move(c));
    }
    REQUIRE(cm.visible_count() == 300u);
    REQUIRE(cm.total_width() == 30000.0f);

    const auto [first, last] = cm.visible_range(1050.0f, 350.0f); // [1050, 1400) -> cols 10..13
    REQUIRE(first == 10);
    REQUIRE(last == 13);

    REQUIRE(cm.hit_test_column(0.0f) == 0);
    REQUIRE(cm.hit_test_column(99.5f) == 0);
    REQUIRE(cm.hit_test_column(100.0f) == 1);
    REQUIRE(cm.hit_test_column(29999.0f) == 299);
    REQUIRE(cm.hit_test_column(30000.0f) == -1);

    REQUIRE(cm.hit_test_divider(98.0f) == 0);
    REQUIRE(cm.hit_test_divider(103.0f) == 0);
    REQUIRE(cm.hit_test_divider(50.0f) == -1);

    // Move column 0 to display index 2
    cm.set_display_index(0, 2);
    REQUIRE(cm.visible_ordinals()[0] == 1u);
    REQUIRE(cm.visible_ordinals()[1] == 2u);
    REQUIRE(cm.visible_ordinals()[2] == 0u);
}

TEST_CASE("GridSelectionModel merges adjacent rectangular strips in O(1) and supports full-grid SelectAll", "[ui][grid]") {
    using namespace capybaradb::ui::grid;

    GridSelectionModel sel;
    REQUIRE_FALSE(sel.has_selection());

    sel.select_all(50000, 300);
    REQUIRE(sel.ranges().size() == 1);
    REQUIRE(sel.selected_cell_count(50000, 300) == 15000000ull);
    REQUIRE(sel.is_cell_selected(49999, 299));

    // O(1) adjacent strip merge
    sel.clear(true);
    for (int r = 0; r < 10; ++r) {
        sel.add_cell(r, 0);
    }
    REQUIRE(sel.ranges().size() == 1);
    for (int r = 0; r < 10; ++r) {
        sel.add_cell(r, 1);
    }
    REQUIRE(sel.ranges().size() == 1);
    REQUIRE(sel.selected_cell_count(100, 10) == 20ull);
}

TEST_CASE("ResultFormatter builds TSV clipboard payloads with 4 MiB and 250k-cell guards, validates JSON, and compares numeric cells", "[core][result][formatter]") {
    using namespace capybaradb::core;

    QueryExecutionResult qres;
    const std::array<ColumnSpec, 3> cols = {{
        {"id", "int4", LogicalTypeKind::Int, false},
        {"price", "numeric", LogicalTypeKind::Numeric, false},
        {"note", "text", LogicalTypeKind::Text, true},
    }};
    ResultSetStorage* rs = qres.create_result_set(cols);
    REQUIRE(rs != nullptr);
    {
        ResultSetWriter w(qres, *rs, 64ull * 1024 * 1024, 65536u);
        auto add = [&](const char* a, const char* b, const char* c) {
            REQUIRE(w.begin_row());
            if (a) w.write_utf8(0, a, std::strlen(a)); else w.write_null(0);
            if (b) w.write_utf8(1, b, std::strlen(b)); else w.write_null(1);
            if (c) w.write_utf8(2, c, std::strlen(c)); else w.write_null(2);
            (void)w.end_row(false);
        };
        add("10", "9.50", "line1\r\nline2");
        add("2", "100.25", "{\"a\":1}");
        add(nullptr, "-5.00", "plain");
        w.finish();
    }

    // 1. Single cell without headers -> pure value, no CRLF, no quotes
    const std::array<CellRange, 1> r_single = {{CellRange{0, 0, 2, 2}}};
    const auto tsv_single = build_clipboard_tsv(*rs, ClipboardCopyOptions{
        .ranges = r_single, .include_headers = false, .retain_line_breaks = true,
    });
    REQUIRE(tsv_single.has_value());
    REQUIRE(*tsv_single == "line1\r\nline2");

    // 2. Flattened line breaks
    const auto tsv_flat = build_clipboard_tsv(*rs, ClipboardCopyOptions{
        .ranges = r_single, .include_headers = false, .retain_line_breaks = false,
    });
    REQUIRE(tsv_flat.has_value());
    REQUIRE(*tsv_flat == "line1 line2");

    // 3. Multi-cell with headers
    const std::array<CellRange, 1> r_all = {{CellRange{0, 2, 0, 1}}};
    const auto tsv_hdr = build_clipboard_tsv(*rs, ClipboardCopyOptions{
        .ranges = r_all, .include_headers = true, .retain_line_breaks = true,
    });
    REQUIRE(tsv_hdr.has_value());
    REQUIRE(*tsv_hdr == "id\tprice\r\n10\t9.50\r\n2\t100.25\r\nNULL\t-5.00");

    // 4. Numeric and NULL comparison (NULL first in ASC, 2 < 10, -5.00 < 9.50 < 100.25)
    REQUIRE(compare_result_cells(*rs, 2, 1, 0, false) < 0); // NULL < 2
    REQUIRE(compare_result_cells(*rs, 1, 0, 0, false) < 0); // 2 < 10
    REQUIRE(compare_result_cells(*rs, 2, 0, 1, false) < 0); // -5.00 < 9.50
    REQUIRE(compare_result_cells(*rs, 0, 1, 1, false) < 0); // 9.50 < 100.25

    // 5. JSON validation & pretty-printing
    REQUIRE(is_valid_json_document("{\"a\":1}"));
    REQUIRE(is_valid_json_document("[1, 2, 3]"));
    REQUIRE_FALSE(is_valid_json_document("plain"));
    REQUIRE_FALSE(is_valid_json_document("123"));
    const auto pretty = pretty_print_json("{\"a\":1}");
    REQUIRE(pretty.has_value());
    REQUIRE(pretty->find('\n') != std::string::npos);

    // 6. 4 MiB clipboard guard aborts before allocating
    QueryExecutionResult big_q;
    const std::array<ColumnSpec, 1> big_cols = {{{"blob", "text", LogicalTypeKind::Text, false}}};
    ResultSetStorage* brs = big_q.create_result_set(big_cols);
    REQUIRE(brs != nullptr);
    {
        ResultSetWriter w(big_q, *brs, 64ull * 1024 * 1024, 65536u);
        const std::string payload(60000, 'Z');
        for (int r = 0; r < 80; ++r) { // 80 * 60,000 = 4.8 MB > 4 MiB
            REQUIRE(w.begin_row());
            w.write_utf8(0, payload.data(), payload.size());
            (void)w.end_row(false);
        }
        w.finish();
    }
    const std::array<CellRange, 1> r_big = {{CellRange{0, 79, 0, 0}}};
    const auto big_res = build_clipboard_tsv(*brs, ClipboardCopyOptions{.ranges = r_big});
    REQUIRE_FALSE(big_res.has_value());
    REQUIRE(big_res.error().find("4 MiB") != std::string::npos);

    // 7. 250,000 cell guard
    const std::array<CellRange, 1> r_huge_cells = {{CellRange{0, 299999, 0, 0}}};
    // Build a tiny storage with 250,001 rows? Or 1000 rows x 251 cols = 251,000 cells!
    QueryExecutionResult wide_q;
    std::vector<ColumnSpec> wcols(251, ColumnSpec{"c", "int4", LogicalTypeKind::Int, false});
    ResultSetStorage* wrs = wide_q.create_result_set(wcols);
    REQUIRE(wrs != nullptr);
    {
        ResultSetWriter w(wide_q, *wrs, 64ull * 1024 * 1024, 65536u);
        for (int r = 0; r < 1000; ++r) {
            REQUIRE(w.begin_row());
            for (uint32_t c = 0; c < 251; ++c) {
                w.write_utf8(c, "1", 1);
            }
            (void)w.end_row(false);
        }
        w.finish();
    }
    const std::array<CellRange, 1> r_251k = {{CellRange{0, 999, 0, 250}}};
    const auto too_many = build_clipboard_tsv(*wrs, ClipboardCopyOptions{.ranges = r_251k});
    REQUIRE_FALSE(too_many.has_value());
    REQUIRE(too_many.error().find("too many cells") != std::string::npos);
    (void)r_huge_cells;
}
