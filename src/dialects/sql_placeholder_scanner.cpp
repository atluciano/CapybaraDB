#include "dialects/sql_placeholder_scanner.hpp"

#include "dialects/dialect_lex_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace capybaradb::dialects {
namespace {

constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        const unsigned char la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
        const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
        if (la != lb) {
            return false;
        }
    }
    return true;
}

constexpr bool is_ascii_digit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

bool is_exempt_statement(const std::array<std::string_view, 4>& words, size_t word_count) noexcept {
    if (word_count == 0) {
        return false;
    }
    if (iequals_ascii(words[0], "PREPARE")) {
        return true;
    }
    if (!iequals_ascii(words[0], "CREATE")) {
        return false;
    }
    const size_t kind_idx =
        (word_count > 3 && iequals_ascii(words[1], "OR") && iequals_ascii(words[2], "REPLACE")) ? 3u : 1u;
    return word_count > kind_idx &&
           (iequals_ascii(words[kind_idx], "FUNCTION") || iequals_ascii(words[kind_idx], "PROCEDURE"));
}

size_t utf16_length_of_prefix(std::string_view utf8) noexcept {
    size_t units = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        if (b < 0x80u) {
            ++units;
            ++i;
        } else if ((b & 0xE0u) == 0xC0u) {
            ++units;
            i += 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            ++units;
            i += 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            units += 2; // Supplementary plane -> surrogate pair in UTF-16
            i += 4;
        } else {
            ++units;
            ++i;
        }
    }
    return units;
}

size_t skip_quoted(std::string_view sql, size_t open, char quote, bool backslash_escapes) noexcept {
    size_t i = open + 1;
    bool closed = false;
    detail::advance_quoted(sql, i, quote, backslash_escapes, closed);
    return i;
}

} // namespace

std::string UnboundParameterInfo::description() const {
    if (count == 1) {
        return "$" + std::to_string(first_number);
    }
    return "$" + std::to_string(first_number) +
           " (highest $" + std::to_string(highest_number) +
           ", " + std::to_string(count) + " occurrences)";
}

std::optional<UnboundParameterInfo> find_postgres_unbound(std::string_view sql) noexcept {
    bool   found_any       = false;
    size_t first_offset    = 0;
    int    first_number    = 0;
    int    highest         = 0;
    int    total_count     = 0;

    std::array<std::string_view, 4> words{};
    size_t word_count   = 0;
    size_t stmt_first   = 0;
    int    stmt_number  = 0;
    int    stmt_highest = 0;
    int    stmt_count   = 0;

    auto end_statement = [&]() noexcept {
        if (stmt_count > 0 && !is_exempt_statement(words, word_count)) {
            if (!found_any) {
                found_any    = true;
                first_offset = stmt_first;
                first_number = stmt_number;
            }
            highest = std::max(highest, stmt_highest);
            total_count += stmt_count;
        }
        word_count   = 0;
        stmt_first   = 0;
        stmt_number  = 0;
        stmt_highest = 0;
        stmt_count   = 0;
    };

    size_t i = 0;
    while (i < sql.size()) {
        const char c = sql[i];
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int depth = 1;
            i += 2;
            while (i < sql.size() && depth > 0) {
                if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                    ++depth;
                    i += 2;
                } else if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            }
            continue;
        }
        if (c == '\'') {
            i = skip_quoted(sql, i, '\'', false);
            continue;
        }
        if (c == '"') {
            i = skip_quoted(sql, i, '"', false);
            continue;
        }
        if (c == ';') {
            end_statement();
            ++i;
            continue;
        }
        if (c == '$') {
            if (i + 1 < sql.size() && is_ascii_digit(sql[i + 1])) {
                const size_t start = i;
                ++i;
                int64_t num = 0;
                constexpr int64_t kIntMax = std::numeric_limits<int>::max();
                while (i < sql.size() && is_ascii_digit(sql[i])) {
                    if (num < kIntMax) {
                        num = std::min(kIntMax, num * 10 + (sql[i] - '0'));
                    }
                    ++i;
                }
                const int val = static_cast<int>(num);
                if (stmt_count == 0) {
                    stmt_first  = start;
                    stmt_number = val;
                }
                stmt_highest = std::max(stmt_highest, val);
                ++stmt_count;
                continue;
            }
            size_t tag_end = i + 1;
            if (tag_end < sql.size() && sql[tag_end] != '$' && detail::is_sql_ident_start(sql[tag_end])) {
                while (tag_end < sql.size() && detail::is_sql_ident_part(sql[tag_end], false)) {
                    ++tag_end;
                }
            }
            if (tag_end < sql.size() && sql[tag_end] == '$') {
                const std::string_view tag = sql.substr(i, tag_end - i + 1);
                const size_t close = sql.find(tag, tag_end + 1);
                i = (close == std::string_view::npos) ? sql.size() : (close + tag.size());
                continue;
            }
            ++i;
            continue;
        }
        if (detail::is_sql_ident_start(c)) {
            const size_t start = i;
            ++i;
            while (i < sql.size() && detail::is_sql_ident_part(sql[i], true)) {
                ++i;
            }
            if ((c == 'E' || c == 'e') && i == start + 1 && i < sql.size() && sql[i] == '\'') {
                i = skip_quoted(sql, i, '\'', true);
                continue;
            }
            if (word_count < words.size()) {
                words[word_count++] = sql.substr(start, i - start);
            }
            continue;
        }
        ++i;
    }
    end_statement();

    if (!found_any) {
        return std::nullopt;
    }

    UnboundParameterInfo info;
    info.byte_offset    = first_offset;
    info.char_offset    = utf16_length_of_prefix(sql.substr(0, first_offset));
    info.first_number   = first_number;
    info.highest_number = highest;
    info.count          = total_count;
    return info;
}

} // namespace capybaradb::dialects
