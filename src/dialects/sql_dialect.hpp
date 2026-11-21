#pragma once

#include "drivers/common/db_types.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace capybaradb::dialects {

enum class LexicalKind : uint8_t {
    Code              = 0,
    Comment           = 1,
    String            = 2,
    EscapeString      = 3,
    QuotedIdentifier  = 4,
    BracketIdentifier = 5,
    DollarString      = 6,
    Keyword           = 7,
    LocalVariable     = 8,
    GlobalVariable    = 9,
};

/// 64-byte cache-line-sized POD lexical state (zero heap allocations).
struct LexicalState {
    static constexpr size_t kMaxDollarDelimLen = 60u;

    LexicalKind kind             = LexicalKind::Code;
    uint8_t     dollar_delim_len = 0;
    int16_t     comment_depth    = 0;
    char        dollar_delim_buf[kMaxDollarDelimLen]{};

    [[nodiscard]] constexpr std::string_view dollar_delimiter() const noexcept {
        return std::string_view(dollar_delim_buf, dollar_delim_len);
    }

    constexpr void set_dollar_delimiter(std::string_view delim) noexcept {
        const size_t n = (delim.size() <= kMaxDollarDelimLen) ? delim.size() : kMaxDollarDelimLen;
        dollar_delim_len = static_cast<uint8_t>(n);
        for (size_t i = 0; i < n; ++i) {
            dollar_delim_buf[i] = delim[i];
        }
        for (size_t i = n; i < kMaxDollarDelimLen; ++i) {
            dollar_delim_buf[i] = '\0';
        }
    }

    constexpr void clear_dollar_delimiter() noexcept {
        dollar_delim_len = 0;
    }

    [[nodiscard]] constexpr bool is_default_code() const noexcept {
        return kind == LexicalKind::Code && comment_depth == 0 && dollar_delim_len == 0;
    }

    [[nodiscard]] constexpr bool operator==(const LexicalState& other) const noexcept {
        return kind == other.kind &&
               comment_depth == other.comment_depth &&
               dollar_delimiter() == other.dollar_delimiter();
    }

    [[nodiscard]] constexpr int to_block_state_int() const noexcept {
        if (is_default_code()) {
            return 0;
        }
        uint32_t h = 2166136261u;
        h = (h ^ static_cast<uint32_t>(kind)) * 16777619u;
        h = (h ^ static_cast<uint32_t>(static_cast<uint16_t>(comment_depth))) * 16777619u;
        for (uint8_t i = 0; i < dollar_delim_len; ++i) {
            h = (h ^ static_cast<unsigned char>(dollar_delim_buf[i])) * 16777619u;
        }
        const int signed_val = static_cast<int>(h & 0x7FFF'FFFFu);
        return signed_val > 0 ? signed_val : 1;
    }
};
static_assert(std::is_trivially_copyable_v<LexicalState> && sizeof(LexicalState) == 64);

struct TokenSpan {
    uint32_t    start  = 0;
    uint32_t    length = 0;
    LexicalKind kind   = LexicalKind::Code;
};
static_assert(std::is_trivially_copyable_v<TokenSpan>);

/// Direct procedural SQL dialect dispatch (no vtable, no heap allocations).
[[nodiscard]] LexicalState scan_sql_line(
    drivers::EngineKind engine,
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens);

[[nodiscard]] bool is_sql_keyword(
    drivers::EngineKind engine,
    std::string_view word) noexcept;

} // namespace capybaradb::dialects
