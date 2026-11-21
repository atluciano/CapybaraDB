#pragma once

#include "dialects/sql_dialect.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>

namespace capybaradb::dialects::detail {

[[nodiscard]] constexpr bool is_sql_ident_start(char ch) noexcept {
    const unsigned char u = static_cast<unsigned char>(ch);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u >= 0x80u;
}

[[nodiscard]] constexpr bool is_sql_ident_part(char ch, bool allow_dollar) noexcept {
    const unsigned char u = static_cast<unsigned char>(ch);
    return is_sql_ident_start(ch) || (u >= '0' && u <= '9') || (allow_dollar && u == '$');
}

[[nodiscard]] inline bool sorted_ascii_keyword_lookup(
    std::span<const std::string_view> sorted_lower_keywords,
    std::string_view word) noexcept {
    if (word.empty() || word.size() > 63) {
        return false;
    }
    std::array<char, 64> buf{};
    for (size_t i = 0; i < word.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(word[i]);
        if (ch >= 0x80u) {
            return false;
        }
        buf[i] = static_cast<char>(std::tolower(ch));
    }
    const std::string_view lower(buf.data(), word.size());
    return std::binary_search(sorted_lower_keywords.begin(), sorted_lower_keywords.end(), lower);
}

inline void emit_span(std::vector<TokenSpan>* out, size_t start, size_t end, LexicalKind kind) {
    if (out != nullptr && end > start) {
        out->push_back(TokenSpan{
            static_cast<uint32_t>(start),
            static_cast<uint32_t>(end - start),
            kind,
        });
    }
}

inline void advance_nested_comment(
    std::string_view line,
    size_t& i,
    LexicalState& state,
    size_t start,
    std::vector<TokenSpan>* out_tokens) {
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
        ++state.comment_depth;
        i += 2;
    } else if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/') {
        i += 2;
        --state.comment_depth;
        if (state.comment_depth <= 0) {
            emit_span(out_tokens, start, i, LexicalKind::Comment);
            state = {};
        }
    } else {
        ++i;
    }
}

inline void advance_quoted(
    std::string_view line,
    size_t& i,
    char quote,
    bool backslash_escape,
    bool& closed) noexcept {
    closed = false;
    while (i < line.size()) {
        if (backslash_escape && line[i] == '\\') {
            i = (std::min)(i + 2, line.size());
            continue;
        }
        if (line[i] == quote) {
            if (i + 1 < line.size() && line[i + 1] == quote) {
                i += 2;
                continue;
            }
            ++i;
            closed = true;
            return;
        }
        ++i;
    }
}

} // namespace capybaradb::dialects::detail
