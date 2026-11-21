#include "core/result/result_exporter.hpp"

#include "core/config/connection_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace capybaradb::core {
namespace {

class BufferedFileWriter final {
public:
    BufferedFileWriter(const std::filesystem::path& path, size_t capacity)
        : out_(path, std::ios::binary | std::ios::trunc) {
        buf_.reserve(capacity);
        cap_ = capacity;
    }

    [[nodiscard]] bool is_open() const noexcept { return out_.is_open(); }
    [[nodiscard]] bool good() const noexcept { return ok_ && out_.good(); }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return total_written_; }

    void write_char(char c) {
        if (!ok_) return;
        buf_.push_back(c);
        if (buf_.size() >= cap_) {
            flush();
        }
    }

    void write_sv(std::string_view sv) {
        if (!ok_ || sv.empty()) return;
        if (buf_.size() + sv.size() > cap_) {
            flush();
            if (!ok_) return;
        }
        if (sv.size() >= cap_) {
            out_.write(sv.data(), static_cast<std::streamsize>(sv.size()));
            if (!out_.good()) {
                ok_ = false;
                return;
            }
            total_written_ += sv.size();
            return;
        }
        buf_.append(sv.data(), sv.size());
    }

    void write_bom() {
        static constexpr char kUtf8Bom[3] = {'\xEF', '\xBB', '\xBF'};
        write_sv(std::string_view{kUtf8Bom, 3});
    }

    void flush() {
        if (!ok_ || buf_.empty()) return;
        out_.write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        if (!out_.good()) {
            ok_ = false;
            return;
        }
        total_written_ += buf_.size();
        buf_.clear();
    }

    bool finalize() {
        flush();
        if (out_.is_open()) {
            out_.flush();
            if (!out_.good()) {
                ok_ = false;
            }
            out_.close();
        }
        return ok_;
    }

private:
    std::ofstream out_;
    std::string   buf_;
    size_t        cap_           = 65536u;
    uint64_t      total_written_ = 0;
    bool          ok_            = true;
};

struct PartialFileGuard final {
    std::filesystem::path path;
    bool                  keep = false;
    ~PartialFileGuard() noexcept {
        if (!keep && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

std::filesystem::path make_partial_path(const std::filesystem::path& destination) {
    std::string uid = generate_uuid();
    std::erase(uid, '-');
    if (uid.empty()) {
        uid = "tmp";
    }
    return destination.parent_path() / (destination.filename().string() + "." + uid + ".partial");
}

std::expected<void, std::string> finalize_atomic_rename(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    bool overwrite) {
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(destination, ec)) {
        return std::unexpected(std::string{"The destination file already exists."});
    }
    std::filesystem::rename(partial, destination, ec);
    if (ec) {
        if (overwrite) {
            std::error_code rm_ec;
            std::filesystem::remove(destination, rm_ec);
            ec.clear();
            std::filesystem::rename(partial, destination, ec);
        }
        if (ec) {
            return std::unexpected(std::string{"Could not replace destination file: "} + ec.message());
        }
    }
    return {};
}

void write_csv_quoted(BufferedFileWriter& w, std::string_view sv, bool retain_line_breaks) {
    w.write_char('"');
    for (size_t i = 0; i < sv.size(); ++i) {
        const char ch = sv[i];
        if (ch == '"') {
            w.write_sv("\"\"");
        } else if (!retain_line_breaks && (ch == '\r' || ch == '\n')) {
            if (ch == '\r' && i + 1 < sv.size() && sv[i + 1] == '\n') {
                ++i;
            }
            w.write_char(' ');
        } else {
            w.write_char(ch);
        }
    }
    w.write_char('"');
}

void write_json_string(BufferedFileWriter& w, std::string_view sv, bool retain_line_breaks) {
    w.write_char('"');
    for (size_t i = 0; i < sv.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sv[i]);
        if (!retain_line_breaks && (c == '\r' || c == '\n')) {
            if (c == '\r' && i + 1 < sv.size() && sv[i + 1] == '\n') {
                ++i;
            }
            w.write_char(' ');
            continue;
        }
        switch (c) {
        case '"':  w.write_sv("\\\""); break;
        case '\\': w.write_sv("\\\\"); break;
        case '\b': w.write_sv("\\b");  break;
        case '\f': w.write_sv("\\f");  break;
        case '\n': w.write_sv("\\n");  break;
        case '\r': w.write_sv("\\r");  break;
        case '\t': w.write_sv("\\t");  break;
        default:
            if (c < 0x20u) {
                char esc[7]{};
                std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                w.write_sv(std::string_view{esc, 6});
            } else {
                w.write_char(static_cast<char>(c));
            }
            break;
        }
    }
    w.write_char('"');
}

uint64_t resolve_phys_row(uint64_t view_row, const std::vector<uint32_t>& row_order) noexcept {
    if (!row_order.empty() && view_row < row_order.size()) {
        return row_order[static_cast<size_t>(view_row)];
    }
    return view_row;
}

void write_crlf_text(BufferedFileWriter& w, std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            w.write_sv("\r\n");
            ++i;
        } else if (text[i] == '\n') {
            w.write_sv("\r\n");
            ++i;
        } else {
            const size_t start = i;
            while (i < text.size() && text[i] != '\r' && text[i] != '\n') {
                ++i;
            }
            w.write_sv(text.substr(start, i - start));
        }
    }
}

void write_spaces(BufferedFileWriter& w, size_t count) {
    static constexpr char kSpaces[64] = {
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    };
    while (count > 0) {
        const size_t chunk = (std::min)(count, sizeof(kSpaces));
        w.write_sv(std::string_view{kSpaces, chunk});
        count -= chunk;
    }
}

void write_dashes(BufferedFileWriter& w, size_t count) {
    static constexpr char kDashes[64] = {
        '-','-','-','-','-','-','-','-','-','-','-','-','-','-','-','-',
        '-','-','-','-','-','-','-','-','-','-','-','-','-','-','-','-',
        '-','-','-','-','-','-','-','-','-','-','-','-','-','-','-','-',
        '-','-','-','-','-','-','-','-','-','-','-','-','-','-','-','-',
    };
    while (count > 0) {
        const size_t chunk = (std::min)(count, sizeof(kDashes));
        w.write_sv(std::string_view{kDashes, chunk});
        count -= chunk;
    }
}

} // namespace

std::expected<ExportSummary, std::string> export_result_set(
    const ResultSetStorage& rs,
    const std::filesystem::path& destination,
    const ExportOptions& opts,
    CancelToken& cancel,
    ExportProgressFn on_progress,
    void* progress_user_data) {
    if (destination.empty()) {
        return std::unexpected(std::string{"Choose a destination file path."});
    }
    if (opts.format == ExportFormat::Csv &&
        opts.csv_delimiter != ',' && opts.csv_delimiter != ';' && opts.csv_delimiter != '\t') {
        return std::unexpected(std::string{"Choose comma, semicolon or tab as the CSV delimiter."});
    }
    if (cancel.is_canceled()) {
        return std::unexpected(std::string{"Export canceled."});
    }

    std::error_code ec;
    if (!opts.overwrite && std::filesystem::exists(destination, ec)) {
        return std::unexpected(std::string{"The destination file already exists."});
    }
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
    }

    const uint64_t total_rows = rs.published_rows.load(std::memory_order_acquire);
    const uint32_t col_count  = rs.col_count;

    // Resolve column display order
    std::vector<uint32_t> col_order;
    if (!opts.display_order.empty()) {
        for (const uint32_t ord : opts.display_order) {
            if (ord < col_count) {
                col_order.push_back(ord);
            }
        }
    }
    if (col_order.empty()) {
        col_order.resize(col_count);
        for (uint32_t c = 0; c < col_count; ++c) {
            col_order[c] = c;
        }
    }

    // Resolve view rows to export (streaming directly without duplicating cell strings!)
    bool use_selected_list = false;
    std::vector<uint64_t> selected_view_rows;
    if (opts.scope == ExportScope::SelectedRows) {
        use_selected_list = true;
        std::unordered_set<uint64_t> row_set;
        for (const CellRange& rng : opts.selected_ranges) {
            if (!rng.is_valid()) continue;
            const int64_t r0 = (std::max<int64_t>)(0, rng.row_start);
            const int64_t r1 = (std::min<int64_t>)(static_cast<int64_t>(total_rows) - 1, rng.row_end);
            for (int64_t r = r0; r <= r1; ++r) {
                row_set.insert(static_cast<uint64_t>(r));
            }
        }
        selected_view_rows.assign(row_set.begin(), row_set.end());
        std::sort(selected_view_rows.begin(), selected_view_rows.end());
    }

    const uint64_t rows_to_write = use_selected_list ? static_cast<uint64_t>(selected_view_rows.size()) : total_rows;

    const std::filesystem::path partial_path = make_partial_path(destination);
    PartialFileGuard partial_guard{partial_path, false};

    BufferedFileWriter writer(partial_path, kExportStreamBufferBytes);
    if (!writer.is_open()) {
        return std::unexpected(std::string{"Could not open destination file for writing."});
    }

    uint64_t written_rows = 0;

    if (opts.format == ExportFormat::Csv) {
        if (opts.utf8_bom) {
            writer.write_bom();
        }
        if (opts.include_headers && rs.columns != nullptr && !col_order.empty()) {
            for (size_t i = 0; i < col_order.size(); ++i) {
                if (i > 0) writer.write_char(opts.csv_delimiter);
                write_csv_quoted(writer, rs.columns[col_order[i]].name, opts.retain_line_breaks);
            }
            writer.write_sv("\r\n");
        }

        for (uint64_t idx = 0; idx < rows_to_write; ++idx) {
            if ((idx & 127ull) == 0ull) {
                if (cancel.is_canceled()) {
                    return std::unexpected(std::string{"Export canceled."});
                }
                if (on_progress != nullptr && idx > 0) {
                    on_progress(written_rows, rows_to_write, progress_user_data);
                }
            }
            const uint64_t view_row = use_selected_list ? selected_view_rows[static_cast<size_t>(idx)] : idx;
            const uint64_t phys_row = resolve_phys_row(view_row, opts.row_order);
            const auto [chunk, local_r] = rs.locate_row(phys_row);
            if (chunk == nullptr) continue;

            for (size_t ci = 0; ci < col_order.size(); ++ci) {
                if (ci > 0) writer.write_char(opts.csv_delimiter);
                const uint32_t ord = col_order[ci];
                if (ord >= chunk->col_count) {
                    writer.write_sv("\\N");
                    continue;
                }
                const CellDesc& desc = chunk->cell(local_r, ord);
                if (desc.is_null()) {
                    writer.write_sv("\\N");
                } else {
                    write_csv_quoted(writer, chunk->text_of(desc), opts.retain_line_breaks);
                }
            }
            writer.write_sv("\r\n");
            ++written_rows;
            if (!writer.good()) {
                return std::unexpected(std::string{"Write error while exporting CSV."});
            }
        }
    } else {
        // Streaming JSON: {"columns":[...],"rows":[[...],...]}
        writer.write_sv("{\"columns\":[");
        if (rs.columns != nullptr) {
            for (size_t i = 0; i < col_order.size(); ++i) {
                if (i > 0) writer.write_char(',');
                write_json_string(writer, rs.columns[col_order[i]].name, opts.retain_line_breaks);
            }
        }
        writer.write_sv("],\"rows\":[");

        for (uint64_t idx = 0; idx < rows_to_write; ++idx) {
            if ((idx & 127ull) == 0ull) {
                if (cancel.is_canceled()) {
                    return std::unexpected(std::string{"Export canceled."});
                }
                if (on_progress != nullptr && idx > 0) {
                    on_progress(written_rows, rows_to_write, progress_user_data);
                }
            }
            const uint64_t view_row = use_selected_list ? selected_view_rows[static_cast<size_t>(idx)] : idx;
            const uint64_t phys_row = resolve_phys_row(view_row, opts.row_order);
            const auto [chunk, local_r] = rs.locate_row(phys_row);
            if (chunk == nullptr) continue;

            if (written_rows > 0) {
                writer.write_char(',');
            }
            writer.write_char('[');
            for (size_t ci = 0; ci < col_order.size(); ++ci) {
                if (ci > 0) writer.write_char(',');
                const uint32_t ord = col_order[ci];
                if (ord >= chunk->col_count) {
                    writer.write_sv("null");
                    continue;
                }
                const CellDesc& desc = chunk->cell(local_r, ord);
                if (desc.is_null()) {
                    writer.write_sv("null");
                } else {
                    write_json_string(writer, chunk->text_of(desc), opts.retain_line_breaks);
                }
            }
            writer.write_char(']');
            ++written_rows;
            if (!writer.good()) {
                return std::unexpected(std::string{"Write error while exporting JSON."});
            }
        }
        writer.write_sv("]}");
    }

    if (cancel.is_canceled()) {
        return std::unexpected(std::string{"Export canceled."});
    }
    if (!writer.finalize()) {
        return std::unexpected(std::string{"Failed to flush export output file."});
    }
    if (cancel.is_canceled()) {
        return std::unexpected(std::string{"Export canceled."});
    }
    if (auto mv = finalize_atomic_rename(partial_path, destination, opts.overwrite); !mv) {
        return std::unexpected(mv.error());
    }
    partial_guard.keep = true;

    if (on_progress != nullptr) {
        on_progress(written_rows, rows_to_write, progress_user_data);
    }
    return ExportSummary{
        .rows_written  = written_rows,
        .bytes_written = writer.bytes_written(),
    };
}

std::expected<ExportSummary, std::string> write_results_to_rpt_file(
    const QueryExecutionResult& result,
    const std::filesystem::path& destination,
    const TextResultsFormat& raw_format,
    double elapsed_seconds,
    const std::optional<std::string>& error_text,
    std::string_view query_sql,
    CancelToken& cancel,
    ExportProgressFn on_progress,
    void* progress_user_data) {
    if (destination.empty()) {
        return std::unexpected(std::string{"Choose a results file path."});
    }

    const TextResultsFormat fmt = raw_format.normalized();
    const bool delimited = fmt.is_delimited();
    const char sep       = delimited ? fmt.delimiter_char() : ' ';
    const size_t max_ch  = fmt.max_characters;

    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
    }

    const std::filesystem::path partial_path = make_partial_path(destination);
    PartialFileGuard partial_guard{partial_path, false};

    BufferedFileWriter w(partial_path, kExportStreamBufferBytes);
    if (!w.is_open()) {
        return std::unexpected(std::string{"Could not open results file for writing."});
    }

    // CapybaraDB Results to File (.rpt): UTF-8 with BOM + CRLF line endings
    w.write_bom();

    bool written = false;
    auto separate = [&]() {
        if (written) {
            w.write_sv("\r\n");
        }
        written = true;
    };

    if (fmt.include_query) {
        std::string_view q = query_sql;
        while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front()))) q.remove_prefix(1);
        while (!q.empty() && std::isspace(static_cast<unsigned char>(q.back()))) q.remove_suffix(1);
        if (!q.empty()) {
            separate();
            write_crlf_text(w, q);
            w.write_sv("\r\n");
        }
    }

    const uint32_t rs_count = result.result_set_count.load(std::memory_order_acquire);
    uint64_t total_rows_across_sets = 0;
    for (uint32_t s = 0; s < rs_count && s < QueryExecutionResult::kMaxResultSets; ++s) {
        if (const ResultSetStorage* rs = result.result_sets[s].load(std::memory_order_acquire)) {
            total_rows_across_sets += rs->published_rows.load(std::memory_order_acquire);
        }
    }

    uint64_t total_written_rows = 0;
    for (uint32_t s = 0; s < rs_count && s < QueryExecutionResult::kMaxResultSets; ++s) {
        const ResultSetStorage* rs = result.result_sets[s].load(std::memory_order_acquire);
        if (rs == nullptr) continue;

        const uint32_t ncols = rs->col_count;
        const uint64_t nrows = rs->published_rows.load(std::memory_order_acquire);

        if (ncols > 0 && rs->columns != nullptr) {
            separate();
            std::vector<size_t> widths(ncols, 0);
            std::vector<bool> right_align(ncols, false);
            if (!delimited) {
                for (uint32_t c = 0; c < ncols; ++c) {
                    widths[c]      = (std::min)(max_ch, count_utf8_codepoints(rs->columns[c].name));
                    right_align[c] = fmt.right_align_numeric && is_numeric_column(rs->columns[c]);
                }
                for (uint64_t r = 0; r < nrows; ++r) {
                    if ((r & 511ull) == 0ull && cancel.is_canceled()) {
                        return std::unexpected(std::string{"Results to File canceled."});
                    }
                    const auto [chunk, lr] = rs->locate_row(r);
                    if (chunk == nullptr) continue;
                    for (uint32_t c = 0; c < ncols && c < chunk->col_count; ++c) {
                        const CellDesc& d = chunk->cell(lr, c);
                        const size_t len = d.is_null()
                            ? 4u
                            : (std::min)(max_ch, count_utf8_codepoints(chunk->text_of(d)));
                        widths[c] = (std::max)(widths[c], len);
                    }
                }
                for (uint32_t c = 0; c < ncols; ++c) {
                    if (widths[c] == 0) widths[c] = 1;
                }
            }

            auto emit_cell = [&](const std::string& val, size_t cp_len, uint32_t c) {
                if (c > 0) w.write_char(sep);
                if (delimited) {
                    w.write_sv(val);
                    return;
                }
                const size_t pad = (widths[c] > cp_len) ? (widths[c] - cp_len) : 0u;
                if (right_align[c] && pad > 0) {
                    write_spaces(w, pad);
                }
                w.write_sv(val);
                if (!right_align[c] && c + 1 < ncols && pad > 0) {
                    write_spaces(w, pad);
                }
            };

            if (fmt.include_headers) {
                for (uint32_t c = 0; c < ncols; ++c) {
                    const std::string h = truncate_utf8_codepoints(rs->columns[c].name, max_ch, true);
                    emit_cell(h, count_utf8_codepoints(h), c);
                }
                w.write_sv("\r\n");
                for (uint32_t c = 0; c < ncols; ++c) {
                    if (c > 0) w.write_char(sep);
                    const size_t dlen = delimited
                        ? (std::max<size_t>)(1u, (std::min)(max_ch, count_utf8_codepoints(rs->columns[c].name)))
                        : widths[c];
                    write_dashes(w, dlen);
                }
                w.write_sv("\r\n");
            }

            for (uint64_t r = 0; r < nrows; ++r) {
                if ((r & 255ull) == 0ull) {
                    if (cancel.is_canceled()) {
                        return std::unexpected(std::string{"Results to File canceled."});
                    }
                    if (on_progress != nullptr && total_written_rows > 0) {
                        on_progress(total_written_rows, total_rows_across_sets, progress_user_data);
                    }
                }
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
                            : truncate_utf8_codepoints(chunk->text_of(d), max_ch, true);
                    }
                    emit_cell(val, count_utf8_codepoints(val), c);
                }
                w.write_sv("\r\n");
                ++total_written_rows;
                if (!w.good()) {
                    return std::unexpected(std::string{"Write error while writing .rpt file."});
                }
            }
            if (!fmt.nocount) {
                w.write_sv("\r\n(" + std::to_string(nrows) + " row(s) returned)\r\n");
            }
        } else {
            const int64_t aff = rs->rows_affected.load(std::memory_order_acquire);
            if (aff >= 0 && !fmt.nocount) {
                separate();
                w.write_sv(aff == 1 ? "(1 row affected)\r\n" : ("(" + std::to_string(aff) + " rows affected)\r\n"));
            }
        }
    }

    const int64_t total_aff = result.total_rows_affected.load(std::memory_order_acquire);
    if (rs_count == 0 && total_aff >= 0 && !fmt.nocount) {
        separate();
        w.write_sv(total_aff == 1 ? "(1 row affected)\r\n" : ("(" + std::to_string(total_aff) + " rows affected)\r\n"));
    }

    const auto msgs = result.messages_snapshot();
    for (const MessageEntry& m : msgs) {
        separate();
        if (!m.sql_state.empty()) {
            const char* sev = (m.severity == MessageSeverity::Error) ? "ERROR"
                : (m.severity == MessageSeverity::Warning) ? "WARNING"
                : (m.severity == MessageSeverity::Notice) ? "NOTICE"
                : "INFO";
            w.write_sv(sev);
            w.write_char(' ');
            w.write_sv(m.sql_state);
            w.write_sv(": ");
        }
        write_crlf_text(w, m.text);
        w.write_sv("\r\n");
    }

    if (error_text.has_value() && !error_text->empty()) {
        separate();
        write_crlf_text(w, *error_text);
        w.write_sv("\r\n");
    } else if (!written) {
        w.write_sv("Commands completed successfully.\r\n");
        written = true;
    }

    {
        std::ostringstream el;
        el.setf(std::ios::fixed);
        el.precision(3);
        el << "Elapsed: " << elapsed_seconds << " s.\r\n";
        w.write_sv(el.str());
    }
    if (fmt.show_completion_time) {
        w.write_sv("\r\n");
        w.write_sv(format_completion_time_line());
        w.write_sv("\r\n");
    }

    if (!w.finalize()) {
        return std::unexpected(std::string{"Failed to flush .rpt results file."});
    }
    if (auto mv = finalize_atomic_rename(partial_path, destination, true); !mv) {
        return std::unexpected(mv.error());
    }
    partial_guard.keep = true;

    if (on_progress != nullptr) {
        on_progress(total_written_rows, total_rows_across_sets, progress_user_data);
    }
    return ExportSummary{
        .rows_written  = total_written_rows,
        .bytes_written = w.bytes_written(),
    };
}

} // namespace capybaradb::core
