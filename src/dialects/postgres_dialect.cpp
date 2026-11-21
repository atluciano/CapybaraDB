#include "dialects/postgres_dialect.hpp"

#include "dialects/dialect_lex_utils.hpp"
#include "dialects/sqlite_dialect.hpp"
#include "dialects/tsql_dialect.hpp"

#include <algorithm>
#include <array>

namespace capybaradb::dialects {
namespace {

consteval auto make_sorted_pg_keywords() {
    auto arr = std::to_array<std::string_view>({
        "all", "alter", "analyze", "and", "any", "array", "as", "asc", "asymmetric", "authorization",
        "begin", "between", "bigint", "bigserial", "binary", "bit", "bool", "boolean", "both", "by",
        "call", "cascade", "case", "cast", "char", "character", "check", "coalesce", "collate", "column",
        "comment", "commit", "concurrently", "conflict", "constraint", "copy", "create", "cross", "current",
        "current_catalog", "current_date", "current_role", "current_schema", "current_time", "current_timestamp",
        "current_user", "database", "date", "declare", "default", "deferrable", "deferred", "delete", "desc",
        "distinct", "do", "domain", "double", "drop",
        "else", "end", "enum", "except", "exclude", "exists", "explain", "extension", "extract",
        "false", "fetch", "filter", "first", "float", "following", "for", "foreign", "format", "from", "full", "function",
        "generated", "grant", "greatest", "group", "grouping",
        "having", "hour",
        "identity", "if", "ilike", "in", "index", "inherits", "initially", "inner", "inout", "input",
        "insert", "int", "integer", "intersect", "interval", "into", "is", "isnull", "isolation",
        "join", "json", "json_table", "jsonb",
        "key",
        "language", "lateral", "leading", "least", "left", "level", "like", "limit", "local", "lock", "locked", "login",
        "materialized", "merge", "minute", "month",
        "natural", "next", "no", "not", "nothing", "notify", "notnull", "nowait", "null", "nullif", "nulls", "numeric",
        "offset", "on", "only", "or", "order", "out", "outer", "over", "overlaps", "owned", "owner",
        "parallel", "partition", "placing", "policy", "position", "preceding", "precision", "prepare", "primary", "procedure", "public",
        "range", "read", "real", "recursive", "references", "refresh", "reindex", "release", "rename", "repeatable",
        "replace", "reset", "restart", "restrict", "return", "returning", "returns", "revoke", "right", "role", "rollback", "row", "rows",
        "savepoint", "schema", "second", "security", "select", "sequence", "serial", "serializable", "session", "session_user",
        "set", "sets", "share", "show", "similar", "skip", "smallint", "some", "stable", "start", "statement", "strict",
        "substring", "symmetric", "system",
        "table", "tablesample", "temp", "temporary", "text", "then", "time", "timestamp", "timestamptz", "to",
        "trailing", "transaction", "treat", "trigger", "trim", "true", "truncate", "type",
        "unbounded", "uncommitted", "union", "unique", "unlogged", "update", "user", "using", "uuid",
        "vacuum", "values", "varchar", "variadic", "verbose", "view", "volatile",
        "when", "where", "window", "with", "within", "without", "work", "write",
        "xml", "year", "zone",
    });
    std::ranges::sort(arr);
    return arr;
}

inline constexpr auto kPgKeywords = make_sorted_pg_keywords();

} // namespace

LexicalState scan_postgres_line(
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

        if (state.kind == LexicalKind::DollarString) {
            const std::string_view delim = state.dollar_delimiter();
            const size_t found = line.find(delim, i);
            if (found == std::string_view::npos) {
                i = line.size();
                continue;
            }
            i = found + delim.size();
            detail::emit_span(out_tokens, start, i, LexicalKind::DollarString);
            state = {};
            continue;
        }

        if (state.kind == LexicalKind::String ||
            state.kind == LexicalKind::EscapeString ||
            state.kind == LexicalKind::QuotedIdentifier) {
            const char quote = (state.kind == LexicalKind::QuotedIdentifier) ? '"' : '\'';
            const bool bs_escape = (state.kind == LexicalKind::EscapeString);
            bool closed = false;
            detail::advance_quoted(line, i, quote, bs_escape, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, state.kind);
                state = {};
            }
            continue;
        }

        start = i;
        const char ch = line[i];

        if (i + 1 < line.size() && ch == '-' && line[i + 1] == '-') {
            detail::emit_span(out_tokens, start, line.size(), LexicalKind::Comment);
            return LexicalState{};
        }

        if (i + 1 < line.size() && ch == '/' && line[i + 1] == '*') {
            state.kind = LexicalKind::Comment;
            state.comment_depth = 1;
            i += 2;
            continue;
        }

        if ((ch == 'E' || ch == 'e') && i + 1 < line.size() && line[i + 1] == '\'') {
            state.kind = LexicalKind::EscapeString;
            i += 2;
            continue;
        }

        if (ch == '\'') {
            state.kind = LexicalKind::String;
            ++i;
            continue;
        }

        if (ch == '"') {
            state.kind = LexicalKind::QuotedIdentifier;
            ++i;
            continue;
        }

        if (ch == '$') {
            size_t end = i + 1;
            if (end < line.size() && line[end] != '$' && detail::is_sql_ident_start(line[end])) {
                while (end < line.size() && detail::is_sql_ident_part(line[end], false)) {
                    ++end;
                }
            }
            if (end < line.size() && line[end] == '$' && (end - i + 1) <= LexicalState::kMaxDollarDelimLen) {
                state.kind = LexicalKind::DollarString;
                state.comment_depth = 0;
                state.set_dollar_delimiter(line.substr(i, end - i + 1));
                i = end + 1;
                continue;
            }
        }

        if (detail::is_sql_ident_start(ch)) {
            ++i;
            while (i < line.size() && detail::is_sql_ident_part(line[i], true)) {
                ++i;
            }
            const std::string_view word = line.substr(start, i - start);
            if (is_postgres_keyword(word)) {
                detail::emit_span(out_tokens, start, i, LexicalKind::Keyword);
            }
            continue;
        }

        ++i;
    }

    if (state.kind != LexicalKind::Code) {
        detail::emit_span(out_tokens, start, i, state.kind);
    }
    return state;
}

bool is_postgres_keyword(std::string_view word) noexcept {
    return detail::sorted_ascii_keyword_lookup(kPgKeywords, word);
}

LexicalState scan_sql_line(
    drivers::EngineKind engine,
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens) {
    switch (engine) {
    case drivers::EngineKind::SqlServer:
        return scan_tsql_line(line, initial_state, out_tokens);
    case drivers::EngineKind::Sqlite:
        return scan_sqlite_line(line, initial_state, out_tokens);
    case drivers::EngineKind::PostgreSql:
    default:
        return scan_postgres_line(line, initial_state, out_tokens);
    }
}

bool is_sql_keyword(drivers::EngineKind engine, std::string_view word) noexcept {
    switch (engine) {
    case drivers::EngineKind::SqlServer:
        return is_tsql_keyword(word);
    case drivers::EngineKind::Sqlite:
        return is_sqlite_keyword(word);
    case drivers::EngineKind::PostgreSql:
    default:
        return is_postgres_keyword(word);
    }
}

} // namespace capybaradb::dialects
