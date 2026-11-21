#include "dialects/tsql_dialect.hpp"

#include "dialects/dialect_lex_utils.hpp"

#include <algorithm>
#include <array>

namespace capybaradb::dialects {
namespace {

consteval auto make_sorted_tsql_keywords() {
    auto arr = std::to_array<std::string_view>({
        "add", "all", "alter", "and", "any", "apply", "as", "asc", "authorization",
        "backup", "begin", "between", "bigint", "binary", "bit", "break", "browse", "bulk", "by",
        "cascade", "case", "cast", "catch", "char", "check", "checkpoint", "close", "clustered",
        "coalesce", "collate", "column", "commit", "compute", "constraint", "contains", "containstable",
        "continue", "convert", "create", "cross", "current", "current_date", "current_time",
        "current_timestamp", "current_user", "cursor",
        "database", "date", "datetime", "datetime2", "datetimeoffset", "dbcc", "deallocate",
        "decimal", "declare", "default", "delete", "deny", "desc", "disk", "distinct", "distributed",
        "double", "drop", "dump",
        "else", "end", "errlvl", "escape", "except", "exec", "execute", "exists", "exit", "external",
        "fetch", "file", "fillfactor", "float", "for", "foreign", "freetext", "freetexttable", "from", "full", "function",
        "go", "goto", "grant", "group",
        "having", "holdlock",
        "identity", "identity_insert", "identitycol", "if", "in", "index", "inner", "insert", "int",
        "intersect", "into", "is", "isolation",
        "join", "json",
        "key", "kill",
        "left", "level", "like", "lineno", "load",
        "matched", "merge", "money",
        "national", "nchar", "nocount", "nocheck", "nonclustered", "not", "ntext", "null", "nullif", "numeric", "nvarchar",
        "of", "off", "offsets", "on", "open", "opendatasource", "openquery", "openrowset", "openxml",
        "option", "or", "order", "outer", "output", "over",
        "parseonly", "percent", "pivot", "plan", "precision", "primary", "print", "proc", "procedure", "public",
        "raiserror", "read", "readtext", "real", "reconfigure", "references", "replication", "restore",
        "restrict", "return", "returns", "revert", "revoke", "right", "rollback", "rowcount", "rowguidcol", "rule",
        "save", "schema", "securityaudit", "select", "semantickeyphrasetable", "semanticsimilaritydetailstable",
        "semanticsimilaritytable", "sequence", "session_user", "set", "setuser", "showplan_xml", "shutdown",
        "smalldatetime", "smallint", "smallmoney", "some", "statistics", "system_user",
        "table", "tablesample", "text", "textsize", "then", "throw", "time", "tinyint", "to", "top",
        "tran", "transaction", "trigger", "truncate", "try", "try_cast", "try_convert", "tsequal",
        "union", "unique", "uniqueidentifier", "unpivot", "update", "updatetext", "use", "user", "using",
        "values", "varbinary", "varchar", "varying", "view",
        "waitfor", "when", "where", "while", "with", "within", "writetext",
        "xml",
    });
    std::ranges::sort(arr);
    return arr;
}

inline constexpr auto kTSqlKeywords = make_sorted_tsql_keywords();

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

} // namespace

LexicalState scan_tsql_line(
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

        if (state.kind == LexicalKind::BracketIdentifier) {
            bool closed = false;
            advance_brackets(line, i, closed);
            if (closed) {
                detail::emit_span(out_tokens, start, i, LexicalKind::BracketIdentifier);
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

        if ((ch == 'N' || ch == 'n') && i + 1 < line.size() && line[i + 1] == '\'') {
            state.kind = LexicalKind::String;
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

        if (ch == '[') {
            state.kind = LexicalKind::BracketIdentifier;
            ++i;
            continue;
        }

        if (ch == '@') {
            const bool is_global = (i + 1 < line.size() && line[i + 1] == '@');
            size_t end = i + (is_global ? 2 : 1);
            if (end < line.size() && detail::is_sql_ident_start(line[end])) {
                ++end;
                while (end < line.size() && (detail::is_sql_ident_part(line[end], true) || line[end] == '#')) {
                    ++end;
                }
                detail::emit_span(
                    out_tokens,
                    start,
                    end,
                    is_global ? LexicalKind::GlobalVariable : LexicalKind::LocalVariable);
                i = end;
                continue;
            }
        }

        if (detail::is_sql_ident_start(ch)) {
            ++i;
            while (i < line.size() && detail::is_sql_ident_part(line[i], true)) {
                ++i;
            }
            const std::string_view word = line.substr(start, i - start);
            if (is_tsql_keyword(word)) {
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

bool is_tsql_keyword(std::string_view word) noexcept {
    return detail::sorted_ascii_keyword_lookup(kTSqlKeywords, word);
}

} // namespace capybaradb::dialects
