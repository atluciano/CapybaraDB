#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace capybaradb::core {

enum CellFlags : uint8_t {
    kCellNone       = 0,
    kCellNull       = 1u << 0, // SQL NULL (renders "NULL" with #FDFBAC background)
    kCellTruncated  = 1u << 1, // Truncated to MaxCellDisplayBytes
    kCellHasNewline = 1u << 2, // Contains '\r', '\n', or '\t'
    kCellNonAscii   = 1u << 3, // Contains byte >= 0x80 (triggers UTF-8/HarfBuzz path)
    kCellJsonLike   = 1u << 4, // Trimmed starts with '{' or '[' in JSON-eligible column
    kCellBinary     = 1u << 5, // Hex representation (\x or 0x)
};

/// Compact 8-byte POD descriptor per cell (8 cells per 64-byte cache line).
struct alignas(8) CellDesc {
    uint32_t offset;           // Byte offset relative to RowChunk::text_base
    uint32_t length_and_flags; // [31:24] = CellFlags, [23:0] = byte_len (up to 16 MiB - 1)

    static constexpr uint32_t kMaxLength = 0x00FF'FFFFu;

    [[nodiscard]] constexpr uint32_t byte_len() const noexcept {
        return length_and_flags & kMaxLength;
    }
    [[nodiscard]] constexpr uint8_t flags() const noexcept {
        return static_cast<uint8_t>(length_and_flags >> 24);
    }
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return (flags() & kCellNull) != 0;
    }

    static constexpr CellDesc make(uint32_t off, uint32_t len, uint8_t flg) noexcept {
        return CellDesc{off, (static_cast<uint32_t>(flg) << 24) | (len & kMaxLength)};
    }
    static constexpr CellDesc make_null() noexcept {
        return CellDesc{0u, static_cast<uint32_t>(kCellNull) << 24};
    }
};
static_assert(sizeof(CellDesc) == 8 && alignof(CellDesc) == 8);
static_assert(std::is_trivially_copyable_v<CellDesc>);

/// Immutable published batch of contiguous rows with flat CellDesc[] and contiguous UTF-8 TextSlab.
struct RowChunk {
    uint64_t  first_row_index; // Global 0-based index of the first row in this chunk
    uint32_t  row_count;       // Actual rows stored in this chunk
    uint32_t  col_count;       // Local copy for cache-friendly indexing
    CellDesc* cells;           // Contiguous [row_count * col_count] in Arena
    char*     text_base;       // Contiguous UTF-8 bytes of all cells in this chunk in Arena
    uint32_t  text_bytes;      // Valid byte length of text_base

    [[nodiscard]] constexpr const CellDesc& cell(uint32_t local_row, uint32_t col) const noexcept {
        return cells[static_cast<size_t>(local_row) * col_count + col];
    }

    [[nodiscard]] constexpr std::string_view text_of(const CellDesc& d) const noexcept {
        return d.is_null() ? std::string_view{"NULL", 4}
                           : std::string_view{text_base + d.offset, d.byte_len()};
    }
};
static_assert(std::is_trivially_copyable_v<RowChunk>);

/// Single-pass copy + CellFlags classifier (newline/tab, non-ASCII, JSON-like, binary).
[[nodiscard]] inline uint8_t copy_and_classify_utf8(
    char* dst,
    const char* src,
    uint32_t len,
    bool may_hold_json,
    bool is_binary) noexcept {
    uint8_t flags = is_binary ? kCellBinary : kCellNone;
    bool seen_non_ws = false;
    for (uint32_t i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        dst[i] = static_cast<char>(ch);
        if (ch >= 0x80u) {
            flags |= kCellNonAscii;
        } else if (ch == '\r' || ch == '\n' || ch == '\t') {
            flags |= kCellHasNewline;
        }
        if (may_hold_json && !seen_non_ws) {
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                seen_non_ws = true;
                if (ch == '{' || ch == '[') {
                    flags |= kCellJsonLike;
                }
            }
        }
    }
    return flags;
}

} // namespace capybaradb::core
