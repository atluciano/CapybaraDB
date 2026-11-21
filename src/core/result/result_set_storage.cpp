#include "core/result/result_set_storage.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace capybaradb::core {
namespace {

std::string_view copy_string_to_arena(ArenaAllocator& arena, std::string_view sv) noexcept {
    if (sv.empty()) {
        return {};
    }
    char* dst = arena.alloc_bytes(sv.size());
    if (dst == nullptr) {
        return {};
    }
    std::memcpy(dst, sv.data(), sv.size());
    return std::string_view{dst, sv.size()};
}

size_t utf8_safe_truncate_len(const char* data, size_t max_len) noexcept {
    size_t cut = max_len;
    while (cut > 0 && (static_cast<unsigned char>(data[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return cut;
}

} // namespace

ResultSetStorage* QueryExecutionResult::create_result_set(
    std::span<const ColumnSpec> cols,
    uint32_t rows_per_chunk) noexcept {
    const uint32_t idx = result_set_count.load(std::memory_order_relaxed);
    if (idx >= kMaxResultSets) {
        global_limit_reasons.fetch_or(kLimitResultSets, std::memory_order_acq_rel);
        return nullptr;
    }

    void* raw_rs = arena.alloc(sizeof(ResultSetStorage), alignof(ResultSetStorage));
    if (raw_rs == nullptr) {
        global_limit_reasons.fetch_or(kLimitArenaBytes, std::memory_order_acq_rel);
        return nullptr;
    }
    auto* rs = new (raw_rs) ResultSetStorage{};
    rs->result_index   = idx;
    rs->rows_per_chunk = (rows_per_chunk > 0) ? rows_per_chunk : ResultSetStorage::kDefaultChunkRows;

    size_t n_cols = cols.size();
    if (n_cols > ResultSetStorage::kMaxColumns) {
        n_cols = ResultSetStorage::kMaxColumns;
        rs->add_limit_reason(kLimitColumns);
    }
    rs->col_count = static_cast<uint32_t>(n_cols);

    if (n_cols > 0) {
        rs->columns = arena.alloc_array<ColumnMeta>(n_cols);
        if (rs->columns == nullptr) {
            global_limit_reasons.fetch_or(kLimitArenaBytes, std::memory_order_acq_rel);
            return nullptr;
        }
        for (size_t i = 0; i < n_cols; ++i) {
            rs->columns[i] = ColumnMeta{
                .name          = copy_string_to_arena(arena, cols[i].name),
                .type_name     = copy_string_to_arena(arena, cols[i].type_name),
                .ordinal       = static_cast<uint32_t>(i),
                .logical_type  = cols[i].logical_type,
                .may_hold_json = cols[i].may_hold_json,
            };
        }
    }

    result_sets[idx].store(rs, std::memory_order_release);
    result_set_count.store(idx + 1u, std::memory_order_release);
    return rs;
}

void QueryExecutionResult::append_message(
    MessageSeverity severity,
    std::string_view sql_state,
    std::string_view text,
    int32_t error_pos,
    int32_t native_code) noexcept {
    std::lock_guard<std::mutex> lock(message_mu);

    const uint32_t cur_count = message_count.load(std::memory_order_relaxed);
    if (cur_count >= kMaxMessages) {
        omitted_messages.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    if (severity == MessageSeverity::Notice || severity == MessageSeverity::Info) {
        const size_t entry_len = sql_state.size() + text.size();
        if (notice_items_count >= kMaxNoticeCount ||
            entry_len > kMaxCharsPerNotice ||
            notice_count_bytes + entry_len > kMaxTotalNoticeChars) {
            omitted_messages.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        notice_count_bytes += entry_len;
        ++notice_items_count;
    }

    if (messages == nullptr) {
        messages = arena.alloc_array<MessageEntry>(kMaxMessages);
        if (messages == nullptr) {
            omitted_messages.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        message_capacity = kMaxMessages;
    }

    messages[cur_count] = MessageEntry{
        .severity    = severity,
        .sql_state   = copy_string_to_arena(arena, sql_state),
        .text        = copy_string_to_arena(arena, text),
        .error_pos   = error_pos,
        .native_code = native_code,
    };
    message_count.store(cur_count + 1u, std::memory_order_release);
}

void QueryExecutionResult::add_rows_affected(int64_t affected, ResultSetStorage* rs) noexcept {
    if (affected < 0) {
        return;
    }
    if (rs != nullptr) {
        rs->rows_affected.store(affected, std::memory_order_release);
    }
    int64_t cur = total_rows_affected.load(std::memory_order_acquire);
    while (true) {
        const int64_t next = (cur < 0) ? affected : (cur + affected);
        if (total_rows_affected.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) {
            break;
        }
    }
}

uint8_t QueryExecutionResult::combined_limit_reasons() const noexcept {
    uint8_t reasons = global_limit_reasons.load(std::memory_order_acquire);
    const uint32_t count = result_set_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count && i < kMaxResultSets; ++i) {
        if (const ResultSetStorage* rs = result_sets[i].load(std::memory_order_acquire)) {
            reasons |= rs->limit_reasons.load(std::memory_order_acquire);
        }
    }
    return reasons;
}

bool QueryExecutionResult::any_limit_hit() const noexcept {
    return combined_limit_reasons() != kLimitNone;
}

std::vector<MessageEntry> QueryExecutionResult::messages_snapshot() const {
    const uint32_t count = message_count.load(std::memory_order_acquire);
    if (count == 0 || messages == nullptr) {
        return {};
    }
    std::vector<MessageEntry> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(messages[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ResultSetWriter
// ---------------------------------------------------------------------------

ResultSetWriter::ResultSetWriter(
    QueryExecutionResult& result,
    ResultSetStorage& storage,
    uint64_t max_arena_bytes,
    uint32_t max_cell_bytes) noexcept
    : result_(result)
    , storage_(storage)
    , max_arena_bytes_(max_arena_bytes > 0 ? max_arena_bytes : (1024ull * 1024 * 1024))
    , max_cell_bytes_(max_cell_bytes > 0 ? (std::min)(max_cell_bytes, CellDesc::kMaxLength) : ResultSetStorage::kMaxCellDisplayBytes)
    , col_count_(storage.col_count)
    , rows_per_chunk_(storage.rows_per_chunk > 0 ? storage.rows_per_chunk : ResultSetStorage::kDefaultChunkRows) {
    // Preallocate staging buffers once per resultset so streaming does 0 per-row allocations.
    const size_t cell_cap = static_cast<size_t>(rows_per_chunk_) * col_count_;
    staged_cells_.reserve(cell_cap);
    staged_text_.reserve((std::min<size_t>)(cell_cap * 16u, 1024u * 1024u));
}

bool ResultSetWriter::begin_row() noexcept {
    if (arena_limit_hit_) {
        return false;
    }
    const size_t start = staged_cells_.size();
    staged_cells_.resize(start + col_count_, CellDesc::make_null());
    return true;
}

void ResultSetWriter::write_null(uint32_t col) noexcept {
    if (arena_limit_hit_ || col >= col_count_) {
        return;
    }
    const size_t base = static_cast<size_t>(staged_rows_) * col_count_;
    staged_cells_[base + col] = CellDesc::make_null();
}

void ResultSetWriter::append_utf8_capped(
    uint32_t col,
    const char* data,
    size_t len,
    bool is_binary) noexcept {
    if (arena_limit_hit_ || col >= col_count_) {
        return;
    }
    const size_t base = static_cast<size_t>(staged_rows_) * col_count_;
    bool truncated = false;
    size_t copy_len = len;
    if (copy_len > max_cell_bytes_) {
        copy_len = utf8_safe_truncate_len(data, max_cell_bytes_);
        truncated = true;
        storage_.add_limit_reason(kLimitCellBytes);
    }

    const uint32_t off = static_cast<uint32_t>(staged_text_.size());
    const size_t old_sz = staged_text_.size();
    staged_text_.resize(old_sz + copy_len);

    const bool may_json = (storage_.columns != nullptr && storage_.columns[col].may_hold_json);
    uint8_t flags = copy_len > 0
        ? copy_and_classify_utf8(staged_text_.data() + old_sz, data, static_cast<uint32_t>(copy_len), may_json, is_binary)
        : (is_binary ? kCellBinary : kCellNone);
    if (truncated) {
        flags |= kCellTruncated;
    }

    staged_cells_[base + col] = CellDesc::make(off, static_cast<uint32_t>(copy_len), flags);
}

void ResultSetWriter::write_utf8(
    uint32_t col,
    const char* data,
    size_t len,
    bool is_binary) noexcept {
    if (data == nullptr) {
        append_utf8_capped(col, "", 0, is_binary);
        return;
    }
    append_utf8_capped(col, data, len, is_binary);
}

void ResultSetWriter::write_utf16(
    uint32_t col,
    const char16_t* data,
    size_t code_units) noexcept {
    if (arena_limit_hit_ || col >= col_count_) {
        return;
    }
    const size_t base = static_cast<size_t>(staged_rows_) * col_count_;
    const uint32_t off = static_cast<uint32_t>(staged_text_.size());
    const bool may_json = (storage_.columns != nullptr && storage_.columns[col].may_hold_json);

    uint8_t flags = kCellNone;
    bool seen_non_ws = false;
    bool truncated = false;
    uint32_t written = 0;

    auto emit_byte = [&](uint8_t b) -> bool {
        if (written >= max_cell_bytes_) {
            truncated = true;
            return false;
        }
        staged_text_.push_back(static_cast<char>(b));
        ++written;
        if (b >= 0x80u) {
            flags |= kCellNonAscii;
        } else if (b == '\r' || b == '\n' || b == '\t') {
            flags |= kCellHasNewline;
        }
        if (may_json && !seen_non_ws) {
            if (b != ' ' && b != '\t' && b != '\r' && b != '\n') {
                seen_non_ws = true;
                if (b == '{' || b == '[') {
                    flags |= kCellJsonLike;
                }
            }
        }
        return true;
    };

    for (size_t i = 0; i < code_units && !truncated; ++i) {
        uint32_t cp = static_cast<uint32_t>(data[i]);
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < code_units) {
            const uint32_t low = static_cast<uint32_t>(data[i + 1]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10u) | (low - 0xDC00u));
                ++i;
            }
        }

        if (cp < 0x80u) {
            (void)emit_byte(static_cast<uint8_t>(cp));
        } else if (cp < 0x800u) {
            if (written + 2u > max_cell_bytes_) { truncated = true; break; }
            (void)emit_byte(static_cast<uint8_t>(0xC0u | (cp >> 6u)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            if (written + 3u > max_cell_bytes_) { truncated = true; break; }
            (void)emit_byte(static_cast<uint8_t>(0xE0u | (cp >> 12u)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | ((cp >> 6u) & 0x3Fu)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
        } else {
            if (written + 4u > max_cell_bytes_) { truncated = true; break; }
            (void)emit_byte(static_cast<uint8_t>(0xF0u | (cp >> 18u)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | ((cp >> 12u) & 0x3Fu)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | ((cp >> 6u) & 0x3Fu)));
            (void)emit_byte(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
        }
    }

    if (truncated) {
        flags |= kCellTruncated;
        storage_.add_limit_reason(kLimitCellBytes);
    }
    staged_cells_[base + col] = CellDesc::make(off, written, flags);
}

void ResultSetWriter::write_binary_hex(
    uint32_t col,
    const uint8_t* bytes,
    size_t byte_len,
    std::string_view prefix) noexcept {
    if (arena_limit_hit_ || col >= col_count_) {
        return;
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    const size_t base = static_cast<size_t>(staged_rows_) * col_count_;
    const uint32_t off = static_cast<uint32_t>(staged_text_.size());

    uint8_t flags = kCellBinary;
    uint32_t written = 0;
    for (const char ch : prefix) {
        if (written >= max_cell_bytes_) {
            flags |= kCellTruncated;
            storage_.add_limit_reason(kLimitCellBytes);
            break;
        }
        staged_text_.push_back(ch);
        ++written;
    }

    for (size_t i = 0; i < byte_len; ++i) {
        if (written + 2u > max_cell_bytes_) {
            flags |= kCellTruncated;
            storage_.add_limit_reason(kLimitCellBytes);
            break;
        }
        staged_text_.push_back(kHex[(bytes[i] >> 4u) & 0x0Fu]);
        staged_text_.push_back(kHex[bytes[i] & 0x0Fu]);
        written += 2u;
    }

    staged_cells_[base + col] = CellDesc::make(off, written, flags);
}

uint64_t ResultSetWriter::end_row(bool force_flush) noexcept {
    if (arena_limit_hit_) {
        return total_rows_;
    }
    ++staged_rows_;
    ++total_rows_;

    if (force_flush ||
        staged_rows_ >= rows_per_chunk_ ||
        staged_text_.size() >= ResultSetStorage::kMaxChunkTextBytes) {
        (void)flush();
    }
    return total_rows_;
}

bool ResultSetWriter::publish_chunk(RowChunk* chunk) noexcept {
    const uint32_t cidx = storage_.chunk_count.load(std::memory_order_relaxed);
    if (cidx >= ResultSetStorage::kMaxChunkDirectory) {
        storage_.add_limit_reason(kLimitArenaBytes);
        arena_limit_hit_ = true;
        return false;
    }

    const uint32_t seg_idx  = cidx >> 10u;
    const uint32_t slot_idx = cidx & 1023u;
    ResultSetStorage::ChunkSegment* seg = storage_.segments[seg_idx].load(std::memory_order_relaxed);
    if (seg == nullptr) {
        void* raw_seg = result_.arena.alloc(sizeof(ResultSetStorage::ChunkSegment), alignof(ResultSetStorage::ChunkSegment));
        if (raw_seg == nullptr) {
            storage_.add_limit_reason(kLimitArenaBytes);
            arena_limit_hit_ = true;
            return false;
        }
        seg = new (raw_seg) ResultSetStorage::ChunkSegment{};
        storage_.segments[seg_idx].store(seg, std::memory_order_release);
    }

    // If a prior chunk was smaller than rows_per_chunk, chunks are non-uniform -> switch locate_row to O(log C)
    if (chunk->first_row_index != static_cast<uint64_t>(cidx) * rows_per_chunk_) {
        storage_.uniform_chunks.store(false, std::memory_order_release);
    }

    seg->entries[slot_idx].store(chunk, std::memory_order_release);
    storage_.chunk_count.store(cidx + 1u, std::memory_order_release);
    storage_.total_text_bytes.fetch_add(chunk->text_bytes, std::memory_order_acq_rel);
    storage_.published_rows.store(chunk->first_row_index + chunk->row_count, std::memory_order_release);
    return true;
}

bool ResultSetWriter::flush() noexcept {
    if (staged_rows_ == 0) {
        return true;
    }
    if (arena_limit_hit_) {
        staged_cells_.clear();
        staged_text_.clear();
        staged_rows_ = 0;
        return false;
    }

    const size_t cell_count = static_cast<size_t>(staged_rows_) * col_count_;
    const size_t needed = sizeof(RowChunk) + cell_count * sizeof(CellDesc) + staged_text_.size();
    if (result_.arena.bytes_used() + needed > max_arena_bytes_) {
        storage_.add_limit_reason(kLimitArenaBytes);
        arena_limit_hit_ = true;
        staged_cells_.clear();
        staged_text_.clear();
        staged_rows_ = 0;
        return false;
    }

    RowChunk* chunk = result_.arena.alloc_array<RowChunk>(1);
    CellDesc* cells = (cell_count > 0) ? result_.arena.alloc_array<CellDesc>(cell_count) : nullptr;
    char* text_base = (!staged_text_.empty()) ? result_.arena.alloc_bytes(staged_text_.size()) : nullptr;

    if (chunk == nullptr || (cell_count > 0 && cells == nullptr) || (!staged_text_.empty() && text_base == nullptr)) {
        storage_.add_limit_reason(kLimitArenaBytes);
        arena_limit_hit_ = true;
        staged_cells_.clear();
        staged_text_.clear();
        staged_rows_ = 0;
        return false;
    }

    if (cell_count > 0) {
        std::memcpy(cells, staged_cells_.data(), cell_count * sizeof(CellDesc));
    }
    if (!staged_text_.empty()) {
        std::memcpy(text_base, staged_text_.data(), staged_text_.size());
    }

    chunk->first_row_index = storage_.published_rows.load(std::memory_order_relaxed);
    chunk->row_count       = staged_rows_;
    chunk->col_count       = col_count_;
    chunk->cells           = cells;
    chunk->text_base       = text_base;
    chunk->text_bytes      = static_cast<uint32_t>(staged_text_.size());

    const uint32_t flushed_rows = staged_rows_;
    staged_cells_.clear();
    staged_text_.clear();
    staged_rows_ = 0;

    if (!publish_chunk(chunk)) {
        return false;
    }
    result_.total_rows_read.fetch_add(flushed_rows, std::memory_order_acq_rel);
    return true;
}

void ResultSetWriter::finish() noexcept {
    (void)flush();
    storage_.complete.store(true, std::memory_order_release);
}

} // namespace capybaradb::core
