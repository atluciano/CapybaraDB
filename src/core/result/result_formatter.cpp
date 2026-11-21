#include "core/result/result_formatter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace capybaradb::core {
namespace {

size_t measure_cell_text(std::string_view sv, bool retain_line_breaks) noexcept {
    if (retain_line_breaks) {
        return sv.size();
    }
    size_t out_len = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        const char ch = sv[i];
        if (ch == '\r') {
            if (i + 1 < sv.size() && sv[i + 1] == '\n') {
                ++i;
            }
            ++out_len; // single space
        } else if (ch == '\n') {
            ++out_len;
        } else {
            ++out_len;
        }
    }
    return out_len;
}

void append_cell_text(std::string& dst, std::string_view sv, bool retain_line_breaks) {
    if (retain_line_breaks) {
        dst.append(sv.data(), sv.size());
        return;
    }
    for (size_t i = 0; i < sv.size(); ++i) {
        const char ch = sv[i];
        if (ch == '\r') {
            if (i + 1 < sv.size() && sv[i + 1] == '\n') {
                ++i;
            }
            dst.push_back(' ');
        } else if (ch == '\n') {
            dst.push_back(' ');
        } else {
            dst.push_back(ch);
        }
    }
}

uint64_t resolve_physical_row(uint64_t view_row, std::span<const uint32_t> row_order) noexcept {
    if (!row_order.empty() && view_row < row_order.size()) {
        return row_order[view_row];
    }
    return view_row;
}

std::string_view trim_sv(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool is_cell_selected_in(std::span<const CellRange> ranges, int64_t row, int32_t col_ordinal) noexcept {
    for (const CellRange& r : ranges) {
        if (r.contains(row, col_ordinal)) {
            return true;
        }
    }
    return false;
}

int compare_ascii_ci(std::string_view a, std::string_view b) noexcept {
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
    }
    if (a.size() != b.size()) {
        return (a.size() < b.size()) ? -1 : 1;
    }
    return 0;
}

int compare_decimal_strings(std::string_view a, std::string_view b, bool& ok) noexcept {
    a = trim_sv(a);
    b = trim_sv(b);
    if (a.empty() || b.empty()) {
        ok = false;
        return 0;
    }

    auto parse_parts = [](std::string_view s, bool& neg, std::string_view& int_part, std::string_view& frac_part) -> bool {
        neg = false;
        if (s.front() == '-') {
            neg = true;
            s.remove_prefix(1);
        } else if (s.front() == '+') {
            s.remove_prefix(1);
        }
        // Skip optional leading currency symbol (e.g. PostgreSQL/SQL Server money '$12.34' or '-$12.34')
        if (!s.empty() && s.front() == '$') {
            s.remove_prefix(1);
        }
        if (s.empty()) {
            return false;
        }
        const size_t dot = s.find('.');
        int_part  = (dot == std::string_view::npos) ? s : s.substr(0, dot);
        frac_part = (dot == std::string_view::npos) ? std::string_view{} : s.substr(dot + 1);
        if (int_part.empty() && frac_part.empty()) {
            return false;
        }
        for (char c : int_part) {
            if (c < '0' || c > '9') return false;
        }
        for (char c : frac_part) {
            if (c < '0' || c > '9') return false;
        }
        while (!int_part.empty() && int_part.front() == '0') {
            int_part.remove_prefix(1);
        }
        while (!frac_part.empty() && frac_part.back() == '0') {
            frac_part.remove_suffix(1);
        }
        if (int_part.empty() && frac_part.empty()) {
            neg = false; // normalize -0 to +0
        }
        return true;
    };

    bool neg_a = false;
    bool neg_b = false;
    std::string_view ia, fa, ib, fb;
    if (!parse_parts(a, neg_a, ia, fa) || !parse_parts(b, neg_b, ib, fb)) {
        ok = false;
        return 0;
    }
    ok = true;
    if (neg_a != neg_b) {
        return neg_a ? -1 : 1;
    }
    int mag = 0;
    if (ia.size() != ib.size()) {
        mag = (ia.size() < ib.size()) ? -1 : 1;
    } else if (const int ic = ia.compare(ib); ic != 0) {
        mag = (ic < 0) ? -1 : 1;
    } else if (const int fc = fa.compare(fb); fc != 0) {
        mag = (fc < 0) ? -1 : 1;
    }
    return neg_a ? -mag : mag;
}

} // namespace

std::string format_completion_time_line() {
    const auto now = std::chrono::system_clock::now();
    const auto secs_tp = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto subsec_ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now - secs_tp).count() / 100; // 100ns (7 decimal digits)
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    std::tm utc_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
    gmtime_s(&utc_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
    gmtime_r(&tt, &utc_tm);
#endif
    // Compute local UTC offset in minutes cleanly
    const int day_diff = (local_tm.tm_year != utc_tm.tm_year)
        ? ((local_tm.tm_year > utc_tm.tm_year) ? 1 : -1)
        : (local_tm.tm_yday - utc_tm.tm_yday);
    const int offset_min = day_diff * 1440 +
                           (local_tm.tm_hour - utc_tm.tm_hour) * 60 +
                           (local_tm.tm_min - utc_tm.tm_min);
    const char sign = (offset_min >= 0) ? '+' : '-';
    const int abs_min = (offset_min >= 0) ? offset_min : -offset_min;

    char date_buf[32]{};
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &local_tm);
    char full_buf[96]{};
    std::snprintf(
        full_buf,
        sizeof(full_buf),
        "Completion time: %s.%07lld%c%02d:%02d",
        date_buf,
        static_cast<long long>(std::clamp<long long>(subsec_ticks, 0, 9999999)),
        sign,
        abs_min / 60,
        abs_min % 60);
    return std::string{full_buf};
}

std::expected<std::string, std::string> build_clipboard_tsv(
    const ResultSetStorage& rs,
    const ClipboardCopyOptions& opts) {
    const uint64_t total_rows = rs.published_rows.load(std::memory_order_acquire);
    const uint32_t total_cols = rs.col_count;
    if (opts.ranges.empty() || total_rows == 0 || total_cols == 0) {
        return std::unexpected(std::string{"Select result cells to copy."});
    }

    // Build ordered column list in display order
    std::vector<uint32_t> full_order;
    std::span<const uint32_t> order_span = opts.display_order;
    if (order_span.empty()) {
        full_order.resize(total_cols);
        for (uint32_t c = 0; c < total_cols; ++c) {
            full_order[c] = c;
        }
        order_span = full_order;
    }

    const bool single_range = (opts.ranges.size() == 1);
    std::vector<uint64_t> selected_rows;
    std::vector<uint32_t> selected_cols;

    uint64_t r0 = 0;
    uint64_t r1 = 0;
    if (single_range) {
        const CellRange& rng = opts.ranges[0];
        if (!rng.is_valid()) {
            return std::unexpected(std::string{"Select result cells to copy."});
        }
        const int64_t cr0 = (std::max<int64_t>)(0, rng.row_start);
        const int64_t cr1 = (std::min<int64_t>)(static_cast<int64_t>(total_rows) - 1, rng.row_end);
        const int32_t cc0 = (std::max<int32_t>)(0, rng.col_start);
        const int32_t cc1 = (std::min<int32_t>)(static_cast<int32_t>(total_cols) - 1, rng.col_end);
        if (cr0 > cr1 || cc0 > cc1) {
            return std::unexpected(std::string{"Select result cells to copy."});
        }
        r0 = static_cast<uint64_t>(cr0);
        r1 = static_cast<uint64_t>(cr1);
        for (const uint32_t ord : order_span) {
            if (static_cast<int32_t>(ord) >= cc0 && static_cast<int32_t>(ord) <= cc1) {
                selected_cols.push_back(ord);
            }
        }
    } else {
        std::unordered_set<uint64_t> row_set;
        std::unordered_set<uint32_t> col_set;
        for (const CellRange& rng : opts.ranges) {
            if (!rng.is_valid()) {
                continue;
            }
            const int64_t cr0 = (std::max<int64_t>)(0, rng.row_start);
            const int64_t cr1 = (std::min<int64_t>)(static_cast<int64_t>(total_rows) - 1, rng.row_end);
            const int32_t cc0 = (std::max<int32_t>)(0, rng.col_start);
            const int32_t cc1 = (std::min<int32_t>)(static_cast<int32_t>(total_cols) - 1, rng.col_end);
            if (cr0 > cr1 || cc0 > cc1) {
                continue;
            }
            const uint64_t span_rows = static_cast<uint64_t>(cr1 - cr0 + 1);
            const uint64_t span_cols = static_cast<uint64_t>(cc1 - cc0 + 1);
            if (span_rows * span_cols > kMaxClipboardCopyCells) {
                return std::unexpected(std::string{"Copy selection spans too many cells. Select fewer cells or export CSV."});
            }
            for (int64_t r = cr0; r <= cr1; ++r) {
                row_set.insert(static_cast<uint64_t>(r));
            }
            for (int32_t c = cc0; c <= cc1; ++c) {
                col_set.insert(static_cast<uint32_t>(c));
            }
        }
        if (row_set.empty() || col_set.empty()) {
            return std::unexpected(std::string{"Select result cells to copy."});
        }
        selected_rows.assign(row_set.begin(), row_set.end());
        std::sort(selected_rows.begin(), selected_rows.end());
        for (const uint32_t ord : order_span) {
            if (col_set.contains(ord)) {
                selected_cols.push_back(ord);
            }
        }
    }

    const uint64_t row_count = single_range ? (r1 - r0 + 1) : static_cast<uint64_t>(selected_rows.size());
    const uint64_t col_count = static_cast<uint64_t>(selected_cols.size());
    if (row_count == 0 || col_count == 0) {
        return std::unexpected(std::string{"Select result cells to copy."});
    }
    if (row_count > kMaxClipboardCopyCells || col_count > kMaxClipboardCopyCells || row_count * col_count > kMaxClipboardCopyCells) {
        return std::unexpected(std::string{"Copy selection spans too many cells. Select fewer cells or export CSV."});
    }

    const auto row_at = [&](uint64_t idx) noexcept -> uint64_t {
        return single_range ? (r0 + idx) : selected_rows[static_cast<size_t>(idx)];
    };

    // Single-cell without headers: exact pure value, no trailing CRLF, no quotes
    const bool single_pure_cell = (row_count == 1 && col_count == 1 && !opts.include_headers);

    // -----------------------------------------------------------------------
    // Pass 1: Exact byte count calculation with ZERO heap allocation
    // -----------------------------------------------------------------------
    size_t exact_bytes = 0;

    if (opts.include_headers) {
        for (size_t c = 0; c < selected_cols.size(); ++c) {
            if (c > 0) {
                ++exact_bytes; // '\t'
            }
            const uint32_t ord = selected_cols[c];
            if (rs.columns != nullptr && ord < rs.col_count) {
                exact_bytes += measure_cell_text(rs.columns[ord].name, opts.retain_line_breaks);
            }
        }
        exact_bytes += 2; // "\r\n"
        if (exact_bytes > kMaxClipboardCopyBytes) {
            return std::unexpected(std::string{"Copy exceeds the 4 MiB limit. Select fewer cells or export CSV."});
        }
    }

    for (uint64_t ri = 0; ri < row_count; ++ri) {
        const uint64_t view_row = row_at(ri);
        const uint64_t phys_row = resolve_physical_row(view_row, opts.row_order);
        const auto [chunk, local_row] = rs.locate_row(phys_row);

        for (size_t ci = 0; ci < selected_cols.size(); ++ci) {
            if (ci > 0) {
                ++exact_bytes; // '\t'
            }
            const uint32_t ord = selected_cols[ci];
            if (!single_range && !is_cell_selected_in(opts.ranges, static_cast<int64_t>(view_row), static_cast<int32_t>(ord))) {
                continue; // unselected cell in sparse bounding box -> empty string
            }
            if (chunk != nullptr && ord < chunk->col_count) {
                const CellDesc& desc = chunk->cell(local_row, ord);
                exact_bytes += measure_cell_text(chunk->text_of(desc), opts.retain_line_breaks);
            }
        }
        if (!single_pure_cell && ri + 1 < row_count) {
            exact_bytes += 2; // "\r\n"
        }
        if (exact_bytes > kMaxClipboardCopyBytes) {
            return std::unexpected(std::string{"Copy exceeds the 4 MiB limit. Select fewer cells or export CSV."});
        }
    }

    // -----------------------------------------------------------------------
    // Pass 2: Single exact reservation and linear copy from Arena string_views
    // -----------------------------------------------------------------------
    std::string out;
    out.reserve(exact_bytes);

    if (opts.include_headers) {
        for (size_t c = 0; c < selected_cols.size(); ++c) {
            if (c > 0) {
                out.push_back('\t');
            }
            const uint32_t ord = selected_cols[c];
            if (rs.columns != nullptr && ord < rs.col_count) {
                append_cell_text(out, rs.columns[ord].name, opts.retain_line_breaks);
            }
        }
        out.append("\r\n", 2);
    }

    for (uint64_t ri = 0; ri < row_count; ++ri) {
        const uint64_t view_row = row_at(ri);
        const uint64_t phys_row = resolve_physical_row(view_row, opts.row_order);
        const auto [chunk, local_row] = rs.locate_row(phys_row);

        for (size_t ci = 0; ci < selected_cols.size(); ++ci) {
            if (ci > 0) {
                out.push_back('\t');
            }
            const uint32_t ord = selected_cols[ci];
            if (!single_range && !is_cell_selected_in(opts.ranges, static_cast<int64_t>(view_row), static_cast<int32_t>(ord))) {
                continue;
            }
            if (chunk != nullptr && ord < chunk->col_count) {
                const CellDesc& desc = chunk->cell(local_row, ord);
                append_cell_text(out, chunk->text_of(desc), opts.retain_line_breaks);
            }
        }
        if (!single_pure_cell && ri + 1 < row_count) {
            out.append("\r\n", 2);
        }
    }

    return out;
}

size_t count_utf8_codepoints(std::string_view text) noexcept {
    size_t cp = 0;
    for (const unsigned char b : text) {
        if ((b & 0xC0u) != 0x80u) {
            ++cp;
        }
    }
    return cp;
}

std::string truncate_utf8_codepoints(
    std::string_view text,
    size_t max_chars,
    bool flatten_line_breaks) {
    std::string out;
    out.reserve((std::min)(text.size(), max_chars * 4u));
    size_t cp_count = 0;
    size_t i = 0;
    while (i < text.size() && cp_count < max_chars) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        if (flatten_line_breaks && (b == '\r' || b == '\n')) {
            if (b == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out.push_back(' ');
            ++cp_count;
            ++i;
            continue;
        }
        size_t seq_len = 1;
        if ((b & 0x80u) == 0u) {
            seq_len = 1;
        } else if ((b & 0xE0u) == 0xC0u) {
            seq_len = 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            seq_len = 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            seq_len = 4;
        }
        if (i + seq_len > text.size()) {
            seq_len = text.size() - i;
        }
        out.append(text.data() + i, seq_len);
        i += seq_len;
        ++cp_count;
    }
    return out;
}

bool is_numeric_column(const ColumnMeta& col) noexcept {
    switch (col.logical_type) {
    case LogicalTypeKind::Int:
    case LogicalTypeKind::Float:
    case LogicalTypeKind::Numeric:
        return true;
    default:
        break;
    }
    std::string_view t = col.type_name;
    const size_t paren = t.find('(');
    if (paren != std::string_view::npos) {
        t = t.substr(0, paren);
    }
    t = trim_sv(t);
    return t == "smallint" || t == "integer" || t == "int" || t == "int2" || t == "int4" || t == "int8" ||
           t == "bigint" || t == "tinyint" || t == "numeric" || t == "decimal" || t == "real" ||
           t == "float" || t == "float4" || t == "float8" || t == "double precision" ||
           t == "money" || t == "smallmoney" || t == "oid";
}

std::string format_results_as_text(
    const QueryExecutionResult& result,
    double elapsed_seconds,
    const TextResultsFormat& raw_format,
    const std::optional<std::string>& error_text,
    std::string_view query_sql,
    uint64_t max_rows_per_result) {
    const TextResultsFormat fmt = raw_format.normalized();
    const bool delimited = fmt.is_delimited();
    const char sep = delimited ? fmt.delimiter_char() : ' ';
    const size_t max_chars = fmt.max_characters;

    std::ostringstream out;
    bool written = false;
    auto separate = [&]() {
        if (written) {
            out << '\n';
        }
        written = true;
    };

    if (fmt.include_query) {
        const std::string_view qtrim = trim_sv(query_sql);
        if (!qtrim.empty()) {
            separate();
            out << qtrim << '\n';
        }
    }

    const uint32_t rs_count = result.result_set_count.load(std::memory_order_acquire);
    for (uint32_t s = 0; s < rs_count && s < QueryExecutionResult::kMaxResultSets; ++s) {
        const ResultSetStorage* rs = result.result_sets[s].load(std::memory_order_acquire);
        if (rs == nullptr) {
            continue;
        }
        const uint32_t ncols = rs->col_count;
        const uint64_t nrows = rs->published_rows.load(std::memory_order_acquire);
        const uint64_t text_rows = (max_rows_per_result > 0) ? (std::min)(nrows, max_rows_per_result) : nrows;

        if (ncols > 0 && rs->columns != nullptr) {
            separate();
            std::vector<size_t> widths(ncols, 0);
            std::vector<bool> right_align(ncols, false);
            if (!delimited) {
                for (uint32_t c = 0; c < ncols; ++c) {
                    widths[c] = (std::min)(max_chars, count_utf8_codepoints(rs->columns[c].name));
                    right_align[c] = fmt.right_align_numeric && is_numeric_column(rs->columns[c]);
                }
                for (uint64_t r = 0; r < text_rows; ++r) {
                    const auto [chunk, lr] = rs->locate_row(r);
                    if (chunk == nullptr) continue;
                    for (uint32_t c = 0; c < ncols && c < chunk->col_count; ++c) {
                        const CellDesc& d = chunk->cell(lr, c);
                        const size_t len = d.is_null()
                            ? 4u
                            : (std::min)(max_chars, count_utf8_codepoints(chunk->text_of(d)));
                        widths[c] = (std::max)(widths[c], len);
                    }
                }
                for (uint32_t c = 0; c < ncols; ++c) {
                    if (widths[c] == 0) widths[c] = 1;
                }
            }

            auto write_padded = [&](const std::string& val, size_t cp_len, uint32_t c) {
                if (c > 0) out << sep;
                if (delimited) {
                    out << val;
                    return;
                }
                const size_t pad = (widths[c] > cp_len) ? (widths[c] - cp_len) : 0u;
                if (right_align[c] && pad > 0) {
                    out << std::string(pad, ' ');
                }
                out << val;
                if (!right_align[c] && c + 1 < ncols && pad > 0) {
                    out << std::string(pad, ' ');
                }
            };

            if (fmt.include_headers) {
                for (uint32_t c = 0; c < ncols; ++c) {
                    const std::string h = truncate_utf8_codepoints(rs->columns[c].name, max_chars, true);
                    write_padded(h, count_utf8_codepoints(h), c);
                }
                out << '\n';
                for (uint32_t c = 0; c < ncols; ++c) {
                    if (c > 0) out << sep;
                    const size_t dash_len = delimited
                        ? (std::max<size_t>)(1u, (std::min)(max_chars, count_utf8_codepoints(rs->columns[c].name)))
                        : widths[c];
                    out << std::string(dash_len, '-');
                }
                out << '\n';
            }

            for (uint64_t r = 0; r < text_rows; ++r) {
                const auto [chunk, lr] = rs->locate_row(r);
                if (chunk == nullptr) continue;
                for (uint32_t c = 0; c < ncols; ++c) {
                    std::string val;
                    if (c >= chunk->col_count) {
                        val = "NULL";
                    } else {
                        const CellDesc& d = chunk->cell(lr, c);
                        val = d.is_null()
                            ? std::string{"NULL"}
                            : truncate_utf8_codepoints(chunk->text_of(d), max_chars, true);
                    }
                    write_padded(val, count_utf8_codepoints(val), c);
                }
                out << '\n';
            }
            if (nrows > text_rows) {
                out << "... (" << (nrows - text_rows) << " more row(s) omitted in text view)\n";
            }
            if (!fmt.nocount) {
                out << "\n(" << nrows << " row(s) returned)\n";
            }
        } else {
            const int64_t aff = rs->rows_affected.load(std::memory_order_acquire);
            if (aff >= 0 && !fmt.nocount) {
                separate();
                out << (aff == 1 ? "(1 row affected)" : ("(" + std::to_string(aff) + " rows affected)")) << '\n';
            }
        }
    }

    const int64_t affected = result.total_rows_affected.load(std::memory_order_acquire);
    if (rs_count == 0 && affected >= 0 && !fmt.nocount) {
        separate();
        out << (affected == 1 ? "(1 row affected)" : ("(" + std::to_string(affected) + " rows affected)")) << '\n';
    }

    const auto msgs = result.messages_snapshot();
    for (const MessageEntry& m : msgs) {
        separate();
        if (!m.sql_state.empty()) {
            const char* sev = (m.severity == MessageSeverity::Error) ? "ERROR"
                : (m.severity == MessageSeverity::Warning) ? "WARNING"
                : (m.severity == MessageSeverity::Notice) ? "NOTICE"
                : "INFO";
            out << sev << ' ' << m.sql_state << ": " << m.text << '\n';
        } else {
            out << m.text << '\n';
        }
    }
    const uint64_t omitted = result.omitted_messages.load(std::memory_order_acquire);
    if (omitted > 0) {
        separate();
        out << omitted
            << " server notice(s) omitted: limit 100 notices, 4096 characters each, 65536 characters total.\n";
    }
    if (error_text.has_value() && !error_text->empty()) {
        separate();
        out << *error_text << '\n';
    } else if (!written) {
        out << "Commands completed successfully.\n";
        written = true;
    }
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "Elapsed: " << elapsed_seconds << " s.\n";
    if (fmt.show_completion_time) {
        out << '\n' << format_completion_time_line() << '\n';
    }
    return out.str();
}

std::string format_results_as_text(
    const QueryExecutionResult& result,
    double elapsed_seconds,
    bool show_completion_time,
    bool nocount,
    const std::optional<std::string>& error_text) {
    TextResultsFormat fmt;
    fmt.show_completion_time = show_completion_time;
    fmt.nocount              = nocount;
    return format_results_as_text(result, elapsed_seconds, fmt, error_text, {}, 10000ull);
}

bool is_valid_json_document(std::string_view text) noexcept {
    const std::string_view trimmed = trim_sv(text);
    if (trimmed.size() < 2) {
        return false;
    }
    const bool is_obj = (trimmed.front() == '{' && trimmed.back() == '}');
    const bool is_arr = (trimmed.front() == '[' && trimmed.back() == ']');
    if (!is_obj && !is_arr) {
        return false;
    }
    return nlohmann::json::accept(trimmed.begin(), trimmed.end());
}

std::optional<std::string> pretty_print_json(std::string_view text) {
    const std::string_view trimmed = trim_sv(text);
    if (trimmed.size() < 2 ||
        !((trimmed.front() == '{' && trimmed.back() == '}') ||
          (trimmed.front() == '[' && trimmed.back() == ']'))) {
        return std::nullopt;
    }
    auto parsed = nlohmann::json::parse(trimmed.begin(), trimmed.end(), nullptr, false);
    if (parsed.is_discarded() || (!parsed.is_object() && !parsed.is_array())) {
        return std::nullopt;
    }
    return parsed.dump(2);
}

int compare_result_cells(
    const ResultSetStorage& rs,
    uint64_t phys_row_a,
    uint64_t phys_row_b,
    uint32_t col_ordinal,
    bool descending) noexcept {
    if (col_ordinal >= rs.col_count) {
        return 0;
    }
    const auto [chunk_a, la] = rs.locate_row(phys_row_a);
    const auto [chunk_b, lb] = rs.locate_row(phys_row_b);
    const bool null_a = (chunk_a == nullptr || col_ordinal >= chunk_a->col_count || chunk_a->cell(la, col_ordinal).is_null());
    const bool null_b = (chunk_b == nullptr || col_ordinal >= chunk_b->col_count || chunk_b->cell(lb, col_ordinal).is_null());

    if (null_a && null_b) {
        return 0;
    }
    if (null_a != null_b) {
        const int base = null_a ? -1 : 1; // NULL sorts first in ASC
        return descending ? -base : base;
    }

    const std::string_view sa = chunk_a->text_of(chunk_a->cell(la, col_ordinal));
    const std::string_view sb = chunk_b->text_of(chunk_b->cell(lb, col_ordinal));
    const LogicalTypeKind kind = (rs.columns != nullptr) ? rs.columns[col_ordinal].logical_type : LogicalTypeKind::Unknown;

    int cmp = 0;
    switch (kind) {
    case LogicalTypeKind::Int: {
        int64_t va = 0;
        int64_t vb = 0;
        const std::string_view ta = trim_sv(sa);
        const std::string_view tb = trim_sv(sb);
        const auto ra = std::from_chars(ta.data(), ta.data() + ta.size(), va);
        const auto rb = std::from_chars(tb.data(), tb.data() + tb.size(), vb);
        if (ra.ec == std::errc{} && ra.ptr == ta.data() + ta.size() &&
            rb.ec == std::errc{} && rb.ptr == tb.data() + tb.size()) {
            cmp = (va < vb) ? -1 : (va > vb ? 1 : 0);
        } else {
            bool dec_ok = false;
            cmp = compare_decimal_strings(sa, sb, dec_ok);
            if (!dec_ok) {
                cmp = compare_ascii_ci(sa, sb);
            }
        }
        break;
    }
    case LogicalTypeKind::Numeric: {
        bool dec_ok = false;
        cmp = compare_decimal_strings(sa, sb, dec_ok);
        if (!dec_ok) {
            cmp = compare_ascii_ci(sa, sb);
        }
        break;
    }
    case LogicalTypeKind::Float: {
        double da = 0.0;
        double db = 0.0;
        const std::string_view ta = trim_sv(sa);
        const std::string_view tb = trim_sv(sb);
        const auto ra = std::from_chars(ta.data(), ta.data() + ta.size(), da);
        const auto rb = std::from_chars(tb.data(), tb.data() + tb.size(), db);
        if (ra.ec == std::errc{} && rb.ec == std::errc{} && !std::isnan(da) && !std::isnan(db)) {
            cmp = (da < db) ? -1 : (da > db ? 1 : 0);
        } else {
            cmp = compare_ascii_ci(sa, sb);
        }
        break;
    }
    case LogicalTypeKind::Bool: {
        auto to_bool = [](std::string_view s) noexcept -> int {
            const std::string_view t = trim_sv(s);
            if (t.empty()) return 0;
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t.front())));
            return (c == 't' || c == '1' || c == 'y') ? 1 : 0;
        };
        const int ba = to_bool(sa);
        const int bb = to_bool(sb);
        cmp = (ba < bb) ? -1 : (ba > bb ? 1 : 0);
        break;
    }
    default:
        cmp = compare_ascii_ci(sa, sb);
        break;
    }

    return descending ? -cmp : cmp;
}

} // namespace capybaradb::core
