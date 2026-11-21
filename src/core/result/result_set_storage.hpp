#pragma once

#include "core/arena/arena_allocator.hpp"
#include "core/plan/execution_plan.hpp"
#include "core/result/cell_desc.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace capybaradb::core {

enum class LogicalTypeKind : uint8_t {
    Unknown = 0,
    Bool,
    Int,
    Float,
    Numeric,
    Text,
    Json,
    Xml,
    Binary,
    Date,
    Time,
    Timestamp,
    Uuid,
    Array,
};

struct ColumnMeta {
    std::string_view name;          // Points to UTF-8 copy inside Arena
    std::string_view type_name;     // e.g. "int4", "jsonb", "nvarchar", "datetime2"
    uint32_t         ordinal;       // 0-based
    LogicalTypeKind  logical_type;  // Used for numeric sort, alignment, JSON link
    bool             may_hold_json; // true for json/jsonb/text/varchar/nvarchar/char/nchar
};
static_assert(std::is_trivially_copyable_v<ColumnMeta>);

struct ColumnSpec {
    std::string_view name;
    std::string_view type_name;
    LogicalTypeKind  logical_type  = LogicalTypeKind::Unknown;
    bool             may_hold_json = false;
};

enum LimitReasonFlags : uint8_t {
    kLimitNone       = 0,
    kLimitResultSets = 1u << 0,
    kLimitColumns    = 1u << 1,
    kLimitCellBytes  = 1u << 2,
    kLimitArenaBytes = 1u << 3,
};

/// Flat, chunked storage for a single ResultSet.
/// Written exclusively by the DB worker thread; read lock-free by the UI thread.
struct ResultSetStorage {
    static constexpr uint32_t kDefaultChunkRows    = 256u;
    static constexpr uint32_t kMaxChunkDirectory   = 65536u;
    static constexpr uint32_t kMaxCellDisplayBytes = 65536u;
    static constexpr uint32_t kMaxColumns          = 4096u;
    static constexpr uint32_t kMaxChunkTextBytes   = 8u * 1024u * 1024u; // 8 MiB

    struct ChunkSegment {
        std::atomic<RowChunk*> entries[1024]{};
    };

    uint32_t    result_index   = 0;
    uint32_t    col_count      = 0;
    ColumnMeta* columns        = nullptr; // Array [col_count] in Arena
    uint32_t    rows_per_chunk = kDefaultChunkRows;

    std::atomic<bool>          uniform_chunks{true};
    std::atomic<ChunkSegment*> segments[64]{};
    std::atomic<uint32_t>      chunk_count{0};
    std::atomic<uint64_t>      published_rows{0};
    std::atomic<uint64_t>      total_text_bytes{0};
    std::atomic<bool>          complete{false};
    std::atomic<int64_t>       rows_affected{-1};
    std::atomic<uint8_t>       limit_reasons{kLimitNone};

    [[nodiscard]] const RowChunk* get_chunk(uint32_t chunk_idx) const noexcept {
        const uint32_t seg_idx  = chunk_idx >> 10u;
        const uint32_t slot_idx = chunk_idx & 1023u;
        if (seg_idx >= 64u) {
            return nullptr;
        }
        const ChunkSegment* seg = segments[seg_idx].load(std::memory_order_acquire);
        if (seg == nullptr) {
            return nullptr;
        }
        return seg->entries[slot_idx].load(std::memory_order_acquire);
    }

    /// Lock-free row lookup for the UI thread: O(1) when chunks are uniform, O(log C) otherwise.
    [[nodiscard]] std::pair<const RowChunk*, uint32_t> locate_row(uint64_t row) const noexcept {
        const uint64_t total = published_rows.load(std::memory_order_acquire);
        if (row >= total) {
            return {nullptr, 0u};
        }
        const uint32_t n_chunks = chunk_count.load(std::memory_order_acquire);
        if (n_chunks == 0) {
            return {nullptr, 0u};
        }

        if (uniform_chunks.load(std::memory_order_acquire)) {
            const uint32_t cidx = static_cast<uint32_t>(row / rows_per_chunk);
            const uint32_t lrow = static_cast<uint32_t>(row % rows_per_chunk);
            if (cidx < n_chunks) {
                const RowChunk* chunk = get_chunk(cidx);
                if (chunk != nullptr && lrow < chunk->row_count && chunk->first_row_index + lrow == row) {
                    return {chunk, lrow};
                }
            }
        }

        // Binary search over chunk first_row_index: O(log C)
        uint32_t lo = 0;
        uint32_t hi = n_chunks;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2u;
            const RowChunk* c = get_chunk(mid);
            if (c == nullptr) {
                hi = mid;
                continue;
            }
            if (row < c->first_row_index) {
                hi = mid;
            } else if (row >= c->first_row_index + c->row_count) {
                lo = mid + 1u;
            } else {
                return {c, static_cast<uint32_t>(row - c->first_row_index)};
            }
        }
        return {nullptr, 0u};
    }

    void add_limit_reason(uint8_t flag) noexcept {
        limit_reasons.fetch_or(flag, std::memory_order_acq_rel);
    }
};

enum class MessageSeverity : uint8_t { Info, Notice, Warning, Error, System };

struct MessageEntry {
    MessageSeverity  severity;
    std::string_view sql_state;
    std::string_view text;
    int32_t          error_pos;
    int32_t          native_code;
};

/// Root object owning the entire Arena and all ResultSets / Messages of a single query execution.
/// Releasing the last std::shared_ptr<QueryExecutionResult> frees all memory in O(block_count).
struct QueryExecutionResult {
    static constexpr uint32_t kMaxResultSets        = 100u;
    static constexpr uint32_t kMaxMessages          = 1000u;
    static constexpr size_t   kMaxMessageTextBytes  = 1024u * 1024u;
    static constexpr size_t   kMaxNoticeCount       = 100u;
    static constexpr size_t   kMaxCharsPerNotice    = 4096u;
    static constexpr size_t   kMaxTotalNoticeChars  = 65536u;

    ArenaAllocator arena;

    std::atomic<ResultSetStorage*> result_sets[kMaxResultSets]{};
    std::atomic<uint32_t>          result_set_count{0};

    // Append-only messages stored in Arena (mutex guards only low-frequency notices/messages)
    MessageEntry*         messages         = nullptr;
    uint32_t              message_capacity = 0;
    std::atomic<uint32_t> message_count{0};
    std::atomic<uint64_t> omitted_messages{0};

    std::atomic<int64_t>  total_rows_affected{-1};
    std::atomic<uint64_t> total_rows_read{0};
    std::atomic<uint8_t>  global_limit_reasons{kLimitNone};
    std::atomic<bool>     finished{false};
    std::atomic<bool>     canceled{false};
    std::atomic<bool>     has_error{false};
    double                elapsed_seconds = 0.0;

    mutable std::mutex            message_mu;
    size_t                        notice_count_bytes = 0;
    size_t                        notice_items_count = 0;
    std::vector<RawExecutionPlan> actual_plans;
    std::string                   actual_plan_unavailable;

    void add_actual_plan(RawExecutionPlan plan) {
        std::lock_guard<std::mutex> lock(message_mu);
        if (actual_plans.size() < 64u) {
            actual_plans.push_back(std::move(plan));
        }
    }

    void set_actual_plan_unavailable(std::string reason) {
        std::lock_guard<std::mutex> lock(message_mu);
        actual_plan_unavailable = std::move(reason);
    }

    [[nodiscard]] std::vector<RawExecutionPlan> actual_plans_snapshot() const {
        std::lock_guard<std::mutex> lock(message_mu);
        return actual_plans;
    }

    [[nodiscard]] std::string actual_plan_unavailable_reason() const {
        std::lock_guard<std::mutex> lock(message_mu);
        return actual_plan_unavailable;
    }

    /// Allocates a new ResultSetStorage + ColumnMeta[] inside this result's Arena.
    /// Returns nullptr if kMaxResultSets is reached or out of memory.
    [[nodiscard]] ResultSetStorage* create_result_set(
        std::span<const ColumnSpec> cols,
        uint32_t rows_per_chunk = ResultSetStorage::kDefaultChunkRows) noexcept;

    /// Appends a bounded server notice or info/error message into the Arena.
    void append_message(
        MessageSeverity severity,
        std::string_view sql_state,
        std::string_view text,
        int32_t error_pos = -1,
        int32_t native_code = 0) noexcept;

    void add_rows_affected(int64_t affected, ResultSetStorage* rs = nullptr) noexcept;

    [[nodiscard]] bool any_limit_hit() const noexcept;
    [[nodiscard]] uint8_t combined_limit_reasons() const noexcept;
    [[nodiscard]] std::vector<MessageEntry> messages_snapshot() const;
};

/// Single-writer chunk staging builder used by drivers (PgDriver / MsSqlDriver) and tests.
/// Allocates reusable scratch vectors ONCE per resultset, packs exact-size RowChunks into the Arena
/// with zero per-row / per-cell heap allocations, and enforces max_arena_bytes / max_cell_bytes.
class ResultSetWriter final {
public:
    ResultSetWriter(
        QueryExecutionResult& result,
        ResultSetStorage& storage,
        uint64_t max_arena_bytes,
        uint32_t max_cell_bytes) noexcept;

    ResultSetWriter(const ResultSetWriter&) = delete;
    ResultSetWriter& operator=(const ResultSetWriter&) = delete;

    /// Begins staging a new row. Returns false if arena byte budget was reached.
    [[nodiscard]] bool begin_row() noexcept;

    /// Writes SQL NULL into column `col` of the current staged row.
    void write_null(uint32_t col) noexcept;

    /// Copies UTF-8 text into the current staged row in a single pass, computing CellFlags.
    void write_utf8(
        uint32_t col,
        const char* data,
        size_t len,
        bool is_binary = false) noexcept;

    /// Transcodes UTF-16LE directly into the current staged row's UTF-8 buffer without heap churn.
    void write_utf16(
        uint32_t col,
        const char16_t* data,
        size_t code_units) noexcept;

    /// Formats raw binary bytes as "0xHEX" (SQL Server) or "\\xHEX" directly into the staged row.
    void write_binary_hex(
        uint32_t col,
        const uint8_t* bytes,
        size_t byte_len,
        std::string_view prefix = "0x") noexcept;

    /// Completes the staged row. Automatically flushes if chunk capacity or 8 MiB text is reached.
    /// Returns the total published + staged row count for this resultset.
    uint64_t end_row(bool force_flush = false) noexcept;

    /// Publishes any pending staged rows into the Arena and updates ResultSetStorage atomics.
    bool flush() noexcept;

    /// Flushes remaining rows and marks ResultSetStorage::complete = true.
    void finish() noexcept;

    [[nodiscard]] bool     arena_limit_hit() const noexcept { return arena_limit_hit_; }
    [[nodiscard]] uint64_t staged_and_published_rows() const noexcept { return total_rows_; }
    [[nodiscard]] uint32_t staged_row_count() const noexcept { return staged_rows_; }

private:
    bool publish_chunk(RowChunk* chunk) noexcept;
    void append_utf8_capped(
        uint32_t col,
        const char* data,
        size_t len,
        bool is_binary) noexcept;

    QueryExecutionResult& result_;
    ResultSetStorage&     storage_;
    uint64_t              max_arena_bytes_ = 1024ull * 1024 * 1024;
    uint32_t              max_cell_bytes_  = ResultSetStorage::kMaxCellDisplayBytes;
    uint32_t              col_count_       = 0;
    uint32_t              rows_per_chunk_  = ResultSetStorage::kDefaultChunkRows;

    std::vector<CellDesc> staged_cells_;
    std::vector<char>     staged_text_;
    uint32_t              staged_rows_     = 0;
    uint64_t              total_rows_      = 0;
    bool                  arena_limit_hit_ = false;
};

} // namespace capybaradb::core
