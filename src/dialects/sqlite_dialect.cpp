#include "dialects/sqlite_dialect.hpp"

#include "dialects/dialect_lex_utils.hpp"

#include <algorithm>
#include <array>

namespace capybaradb::dialects {
namespace {

consteval auto make_sorted_sqlite_keywords() {
    auto arr = std::to_array<std::string_view>({
        "abort", "action", "add", "after", "all", "alter", "always", "analyze", "and", "as", "asc",
        "attach", "autoincrement", "before", "begin", "between", "blob", "by", "cascade", "case",
        "cast", "check", "collate", "column", "commit", "conflict", "constraint", "create", "cross",
        "current", "current_date", "current_time", "current_timestamp", "database", "default", "deferrable",
        "deferred", "delete", "desc", "detach", "distinct", "do", "drop", "each", "else", "end",
        "escape", "except", "exclude", "exclusive", "exists", "explain", "fail", "filter", "first",
        "following", "for", "foreign", "from", "full", "generated", "glob", "group", "groups", "having",
        "if", "ignore", "immediate", "in", "index", "indexed", "initially", "inner", "insert", "instead",
        "integer", "intersect", "into", "is", "isnull", "join", "key", "last", "left", "like", "limit",
        "match", "materialized", "natural", "no", "not", "nothing", "notnull", "null", "nulls", "of",
        "offset", "on", "or", "order", "others", "outer", "over", "partition", "plan", "pragma",
        "preceding", "primary", "query", "raise", "range", "real", "recursive", "references", "regexp",
        "reindex", "release", "rename", "replace", "restrict", "returning", "right", "rollback", "row",
        "rowid", "rows", "savepoint", "select", "set", "table", "temp", "temporary", "text", "then",
        "ties", "to", "transaction", "trigger", "unbounded", "union", "unique", "update", "using",
        "vacuum", "values", "view", "virtual", "when", "where", "window", "with", "without"
    });
    std::ranges::sort(arr);
    return arr;
}

inline constexpr auto kSqliteKeywords = make_sorted_sqlite_keywords();

void advance_brackets(std::string_view line, size_t& i, bool& closed) noexcept {
    closed = false;
    while (i < line.size()) {
        if (line[i] == ']') {
            if (i + 1 < line.size() && line[i + 1] == ']') {
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

void advance_backticks(std::string_view line, size_t& i, bool& closed) noexcept {
    closed = false;
    while (i < line.size()) {
        if (line[i] == '`') {
            if (i + 1 < line.size() && line[i + 1] == '`') {
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

} // namespace

LexicalState scan_sqlite_line(
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens) {
    if (out_tokens != nullptr) {
        out_tokens->clear();
    }

    LexicalState state = initial_state;
    size_t i = 0;
    size_t start = 0;

    while (i < line.size()) {
        if (state.kind == LexicalKind::Comment) {
            detail::advance_nested_comment(line, i, state, start, out_tokens);
            continue;
        }

        if (state.kind == LexicalKind::String || state.kind == LexicalKind::QuotedIdentifier) {
            const char quote = (state.kind == LexicalKind::QuotedIdentifier) ? '"' : '\'';
            bool closed = false;
            detail::advance_quoted(line, i, quote, false, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, state.kind);
                state = {};
            }
            continue;
        }

        const char c = line[i];

        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            detail::emit_span(out_tokens, i, line.size(), LexicalKind::Comment);
            break;
        }

        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            start = i;
            i += 2;
            state.kind = LexicalKind::Comment;
            state.comment_depth = 1;
            detail::advance_nested_comment(line, i, state, start, out_tokens);
            continue;
        }

        if (c == '\'') {
            start = i;
            ++i;
            state.kind = LexicalKind::String;
            bool closed = false;
            detail::advance_quoted(line, i, '\'', false, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, LexicalKind::String);
                state = {};
            }
            continue;
        }

        if (c == '"') {
            start = i;
            ++i;
            state.kind = LexicalKind::QuotedIdentifier;
            bool closed = false;
            detail::advance_quoted(line, i, '"', false, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, LexicalKind::QuotedIdentifier);
                state = {};
            }
            continue;
        }

        if (c == '[') {
            start = i;
            ++i;
            bool closed = false;
            advance_brackets(line, i, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, LexicalKind::BracketIdentifier);
            }
            continue;
        }

        if (c == '`') {
            start = i;
            ++i;
            bool closed = false;
            advance_backticks(line, i, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, LexicalKind::BracketIdentifier);
            }
            continue;
        }

        if (detail::is_sql_ident_start(c)) {
            const size_t word_start = i;
            while (i < line.size() && detail::is_sql_ident_part(line[i], false)) {
                ++i;
            }
            const std::string_view word = line.substr(word_start, i - word_start);
            if (detail::sorted_ascii_keyword_lookup(kSqliteKeywords, word)) {
                detail::emit_span(out_tokens, word_start, i, LexicalKind::Keyword);
            }
            continue;
        }

        ++i;
    }

    if (state.kind != LexicalKind::Code) {
        detail::emit_span(out_tokens, start, line.size(), state.kind);
    }
    return state;
}

bool is_sqlite_keyword(std::string_view word) noexcept {
    return detail::sorted_ascii_keyword_lookup(kSqliteKeywords, word);
}

} // namespace capybaradb::dialects
