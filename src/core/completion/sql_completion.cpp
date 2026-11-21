#include "core/completion/sql_completion.hpp"

#include "core/completion/accent_fold.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace capybaradb::core {
namespace {

template <size_t N>
consteval bool is_sorted_array(const std::array<std::string_view, N>& arr) {
    for (size_t i = 1; i < N; ++i) {
        if (!(arr[i - 1] < arr[i])) {
            return false;
        }
    }
    return true;
}

constexpr auto kPgReserved = std::to_array<std::string_view>({
    "ALL","ANALYSE","ANALYZE","AND","ANY","ARRAY","AS","ASC","ASYMMETRIC","AUTHORIZATION",
    "BINARY","BOTH","CASE","CAST","CHECK","COLLATE","COLLATION","COLUMN","CONCURRENTLY",
    "CONSTRAINT","CREATE","CROSS","CURRENT_CATALOG","CURRENT_DATE","CURRENT_ROLE",
    "CURRENT_SCHEMA","CURRENT_TIME","CURRENT_TIMESTAMP","CURRENT_USER","DEFAULT","DEFERRABLE",
    "DESC","DISTINCT","DO","ELSE","END","EXCEPT","FALSE","FETCH","FOR","FOREIGN","FREEZE",
    "FROM","FULL","GRANT","GROUP","HAVING","ILIKE","IN","INITIALLY","INNER","INTERSECT",
    "INTO","IS","ISNULL","JOIN","LATERAL","LEADING","LEFT","LIKE","LIMIT","LOCALTIME",
    "LOCALTIMESTAMP","NATURAL","NOT","NOTNULL","NULL","OFFSET","ON","ONLY","OR","ORDER",
    "OUTER","OVERLAPS","PLACING","PRIMARY","REFERENCES","RETURNING","RIGHT","SELECT",
    "SESSION_USER","SIMILAR","SOME","SYMMETRIC","SYSTEM_USER","TABLE","TABLESAMPLE","THEN",
    "TO","TRAILING","TRUE","UNION","UNIQUE","USER","USING","VARIADIC","VERBOSE","WHEN",
    "WHERE","WINDOW","WITH"
});
static_assert(is_sorted_array(kPgReserved));

constexpr auto kTsqlReserved = std::to_array<std::string_view>({
    "ADD","ALL","ALTER","AND","ANY","AS","ASC","BEGIN","BETWEEN","BY","CASE","CAST",
    "CHECK","CLOSE","COALESCE","COLLATE","COLUMN","COMMIT","CONSTRAINT","CONTAINS","CONVERT",
    "CREATE","CROSS","CURRENT_DATE","CURRENT_TIME","CURRENT_TIMESTAMP","CURRENT_USER","CURSOR",
    "DATABASE","DECLARE","DEFAULT","DELETE","DESC","DISTINCT","DROP","ELSE","END","EXCEPT",
    "EXEC","EXECUTE","EXISTS","FETCH","FOR","FOREIGN","FROM","FULL","GO","GOTO","GRANT",
    "GROUP","HAVING","IF","IN","INDEX","INNER","INSERT","INTERSECT","INTO","IS","JOIN",
    "KEY","LEFT","LIKE","MERGE","NOT","NULL","NULLIF","OF","OFFSET","ON","OPEN","OPTION",
    "OR","ORDER","OUTER","OVER","PERCENT","PIVOT","PLAN","PRIMARY","PRINT","PROC",
    "PROCEDURE","RETURN","RIGHT","ROLLBACK","SCHEMA","SELECT","SET","SOME","STATISTICS",
    "TABLE","THEN","TO","TOP","TRAN","TRANSACTION","TRIGGER","TRUNCATE","TRY","UNION",
    "UNIQUE","UPDATE","USE","VALUES","VIEW","WHEN","WHERE","WHILE","WITH"
});
static_assert(is_sorted_array(kTsqlReserved));

constexpr auto kPgStatements = std::to_array<std::string_view>({
    "ALTER","ANALYZE","BEGIN","CALL","COMMIT","COPY","CREATE","DELETE","DO","DROP",
    "EXPLAIN","GRANT","INSERT","LOCK","MERGE","REVOKE","ROLLBACK","SELECT","SET","SHOW",
    "TABLE","TRUNCATE","UPDATE","VACUUM","VALUES","WITH"
});
static_assert(is_sorted_array(kPgStatements));

constexpr auto kTsqlStatements = std::to_array<std::string_view>({
    "ALTER","BEGIN","COMMIT","CREATE","DECLARE","DELETE","DROP","EXEC","EXECUTE","GO",
    "GRANT","IF","INSERT","MERGE","PRINT","REVOKE","ROLLBACK","SELECT","SET","TRUNCATE",
    "UPDATE","USE","WHILE","WITH"
});
static_assert(is_sorted_array(kTsqlStatements));

constexpr auto kNotAliasKeywords = std::to_array<std::string_view>({
    "ALTER","BEGIN","COMMIT","DELETE","DROP","EXEC","EXECUTE","EXPLAIN","GO",
    "INSERT","MATCHED","PRINT","ROLLBACK","SET","TRUNCATE","UPDATE","VALUES"
});
static_assert(is_sorted_array(kNotAliasKeywords));

enum class TokKind : uint8_t { Word, Quoted, Symbol };

struct Tok {
    TokKind     kind  = TokKind::Symbol;
    size_t      start = 0;
    size_t      end   = 0;
    std::string text;

    [[nodiscard]] bool is_name() const noexcept { return kind == TokKind::Word || kind == TokKind::Quoted; }

    [[nodiscard]] std::string name_norm(bool is_tsql) const {
        if (kind == TokKind::Word || is_tsql) {
            return fold_accents_lower(text);
        }
        return text;
    }

    [[nodiscard]] std::string upper() const {
        if (kind != TokKind::Word) return {};
        std::string out(text);
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
};

bool is_reserved(std::string_view up, bool is_tsql) noexcept {
    if (is_tsql) {
        return std::binary_search(kTsqlReserved.begin(), kTsqlReserved.end(), up);
    }
    return std::binary_search(kPgReserved.begin(), kPgReserved.end(), up);
}

bool is_not_alias(std::string_view up) noexcept {
    return std::binary_search(kNotAliasKeywords.begin(), kNotAliasKeywords.end(), up);
}

bool is_ident_start_byte(unsigned char c) noexcept {
    return std::isalpha(c) || c == '_' || c >= 0x80u;
}

bool is_ident_part_byte(unsigned char c) noexcept {
    return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80u;
}

std::optional<std::vector<Tok>> tokenize_for_completion(std::string_view text, size_t caret, bool is_tsql) {
    std::vector<Tok> tokens;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        // Line comment --
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            const size_t nl = text.find('\n', i + 2);
            const size_t end = (nl == std::string_view::npos) ? text.size() : nl;
            if (i < caret && caret <= end) return std::nullopt;
            i = end;
            continue;
        }
        // Nested block comment /* ... */
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            int depth = 1;
            size_t j = i + 2;
            while (j < text.size() && depth > 0) {
                if (j + 1 < text.size() && text[j] == '/' && text[j + 1] == '*') {
                    ++depth;
                    j += 2;
                } else if (j + 1 < text.size() && text[j] == '*' && text[j + 1] == '/') {
                    --depth;
                    j += 2;
                } else {
                    ++j;
                }
            }
            if (i < caret && (caret < j || depth > 0)) return std::nullopt;
            i = j;
            continue;
        }
        // String literal '...' or E'...' or N'...'
        if (c == '\'' ||
            ((c == 'e' || c == 'E' || (is_tsql && (c == 'n' || c == 'N'))) &&
             i + 1 < text.size() && text[i + 1] == '\'')) {
            const bool escapes = (!is_tsql && (c == 'e' || c == 'E'));
            size_t j = (c == '\'') ? (i + 1) : (i + 2);
            bool closed = false;
            while (j < text.size()) {
                if (escapes && text[j] == '\\') {
                    j = (std::min)(j + 2, text.size());
                    continue;
                }
                if (text[j] == '\'') {
                    if (j + 1 < text.size() && text[j + 1] == '\'') {
                        j += 2;
                        continue;
                    }
                    ++j;
                    closed = true;
                    break;
                }
                ++j;
            }
            if (i < caret && (caret < j || !closed)) return std::nullopt;
            i = j;
            continue;
        }
        // PG dollar-quoted string $tag$...$tag$
        if (!is_tsql && c == '$') {
            size_t k = i + 1;
            if (k < text.size() && text[k] != '$' && (std::isalpha(static_cast<unsigned char>(text[k])) || text[k] == '_')) {
                while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '_')) ++k;
            }
            if (k < text.size() && text[k] == '$') {
                const std::string_view tag = text.substr(i, k - i + 1);
                const size_t close = text.find(tag, k + 1);
                const size_t end = (close == std::string_view::npos) ? text.size() : (close + tag.size());
                if (i < caret && (caret < end || close == std::string_view::npos)) return std::nullopt;
                i = end;
                continue;
            }
        }
        // Double-quoted identifier "..."
        if (c == '"') {
            size_t j = i + 1;
            std::string name;
            bool closed = false;
            while (j < text.size()) {
                if (text[j] == '"') {
                    if (j + 1 < text.size() && text[j + 1] == '"') {
                        name.push_back('"');
                        j += 2;
                        continue;
                    }
                    ++j;
                    closed = true;
                    break;
                }
                name.push_back(text[j]);
                ++j;
            }
            if (!closed) {
                const size_t nl = text.find('\n', i);
                j = (nl == std::string_view::npos) ? text.size() : nl;
                name.assign(text.substr(i + 1, j - (i + 1)));
            }
            tokens.push_back(Tok{TokKind::Quoted, i, j, std::move(name)});
            i = j;
            continue;
        }
        // T-SQL bracketed identifier [...]
        if (is_tsql && c == '[') {
            size_t j = i + 1;
            std::string name;
            bool closed = false;
            while (j < text.size()) {
                if (text[j] == ']') {
                    if (j + 1 < text.size() && text[j + 1] == ']') {
                        name.push_back(']');
                        j += 2;
                        continue;
                    }
                    ++j;
                    closed = true;
                    break;
                }
                name.push_back(text[j]);
                ++j;
            }
            if (!closed) {
                const size_t nl = text.find('\n', i);
                j = (nl == std::string_view::npos) ? text.size() : nl;
                name.assign(text.substr(i + 1, j - (i + 1)));
            }
            tokens.push_back(Tok{TokKind::Quoted, i, j, std::move(name)});
            i = j;
            continue;
        }
        // Unquoted word (supports UTF-8 accented identifiers)
        if (is_ident_start_byte(c)) {
            size_t j = i + 1;
            while (j < text.size() && is_ident_part_byte(static_cast<unsigned char>(text[j]))) {
                ++j;
            }
            tokens.push_back(Tok{TokKind::Word, i, j, std::string(text.substr(i, j - i))});
            i = j;
            continue;
        }
        tokens.push_back(Tok{TokKind::Symbol, i, i + 1, std::string(1, text[i])});
        ++i;
    }
    return tokens;
}

enum class Place : uint8_t { Start, Rows, Relation, Expression };

Place place_at(const std::vector<Tok>& tokens, ptrdiff_t before, ptrdiff_t first) {
    if (before < first) return Place::Start;
    const std::string up = tokens[static_cast<size_t>(before)].upper();
    if (up == "FROM") {
        return (before > first && tokens[static_cast<size_t>(before - 1)].upper() == "DELETE")
            ? Place::Relation
            : Place::Rows;
    }
    if (up == "JOIN" || up == "LATERAL") return Place::Rows;
    if (up == "UPDATE" || up == "INTO" || up == "TABLE" || up == "ONLY" || up == "TRUNCATE") {
        return Place::Relation;
    }
    if (tokens[static_cast<size_t>(before)].kind != TokKind::Symbol ||
        tokens[static_cast<size_t>(before)].text != ",") {
        return Place::Expression;
    }
    int depth = 0;
    for (ptrdiff_t i = before - 1; i >= first; --i) {
        const Tok& t = tokens[static_cast<size_t>(i)];
        if (t.kind == TokKind::Symbol && t.text == ")") {
            ++depth;
        } else if (t.kind == TokKind::Symbol && t.text == "(") {
            if (depth-- == 0) return Place::Expression;
        } else if (depth == 0) {
            const std::string u = t.upper();
            if (u == "FROM") return Place::Rows;
            if (u == "SELECT" || u == "WHERE" || u == "SET" || u == "ON" ||
                u == "GROUP" || u == "ORDER" || u == "VALUES" || u == "HAVING") {
                return Place::Expression;
            }
        }
    }
    return Place::Expression;
}

struct TableRef {
    std::optional<std::string> schema;
    std::string                name;
    std::optional<std::string> alias;
    size_t                     start = 0;
};

std::vector<TableRef> collect_references(const std::vector<Tok>& stmt, size_t exclude_start, bool is_tsql) {
    std::vector<TableRef> found;
    auto is_sym = [&](size_t idx, const char* s) {
        return idx < stmt.size() && stmt[idx].kind == TokKind::Symbol && stmt[idx].text == s;
    };

    for (size_t i = 0; i < stmt.size(); ++i) {
        const std::string up = stmt[i].upper();
        if (up != "FROM" && up != "JOIN" && up != "UPDATE" && up != "INTO") continue;
        size_t at = i + 1;
        for (;;) {
            while (at < stmt.size() && (stmt[at].upper() == "ONLY" || stmt[at].upper() == "LATERAL")) ++at;
            if (at >= stmt.size() || !stmt[at].is_name() || stmt[at].start == exclude_start ||
                (stmt[at].kind == TokKind::Word && is_reserved(stmt[at].upper(), is_tsql))) {
                break;
            }
            std::optional<std::string> schema;
            std::string name = stmt[at].name_norm(is_tsql);
            const size_t start = stmt[at].start;
            ++at;
            if (is_sym(at, ".") && at + 1 < stmt.size() && stmt[at + 1].is_name() && stmt[at + 1].start != exclude_start) {
                schema = std::move(name);
                name   = stmt[at + 1].name_norm(is_tsql);
                at += 2;
            } else if (is_sym(at, ".")) {
                break;
            }
            if (is_sym(at, "(")) break; // table-valued function
            std::optional<std::string> alias;
            if (at < stmt.size() && stmt[at].upper() == "AS") ++at;
            if (at < stmt.size() && stmt[at].is_name() && stmt[at].start != exclude_start &&
                (stmt[at].kind == TokKind::Quoted || (!is_reserved(stmt[at].upper(), is_tsql) && !is_not_alias(stmt[at].upper())))) {
                alias = stmt[at].name_norm(is_tsql);
                ++at;
            }
            found.push_back(TableRef{std::move(schema), std::move(name), std::move(alias), start});
            if (up == "FROM" && is_sym(at, ",")) {
                ++at;
                continue;
            }
            break;
        }
    }
    return found;
}

bool eq_ident(std::string_view a, std::string_view b, bool is_tsql) {
    if (!is_tsql) return a == b;
    return fold_accents_lower(a) == fold_accents_lower(b);
}

std::optional<std::string> find_schema_in_catalog(const SqlCompletionCatalog& cat, std::string_view qualifier) {
    for (const std::string& sch : cat.schemas) {
        if (eq_ident(sch, qualifier, cat.is_tsql)) return sch;
    }
    return std::nullopt;
}

std::optional<size_t> resolve_relation_index(
    const SqlCompletionCatalog& cat,
    const std::optional<std::string>& schema,
    std::string_view name) {
    if (schema.has_value()) {
        for (size_t i = 0; i < cat.relations.size(); ++i) {
            if (eq_ident(cat.relations[i].schema, *schema, cat.is_tsql) &&
                eq_ident(cat.relations[i].name, name, cat.is_tsql)) {
                return i;
            }
        }
        return std::nullopt;
    }
    for (const std::string& sp : cat.search_path) {
        for (size_t i = 0; i < cat.relations.size(); ++i) {
            if (eq_ident(cat.relations[i].schema, sp, cat.is_tsql) &&
                eq_ident(cat.relations[i].name, name, cat.is_tsql)) {
                return i;
            }
        }
    }
    return std::nullopt;
}

std::string make_join_condition(
    std::string_view left,
    const std::vector<std::string>& left_cols,
    std::string_view right,
    const std::vector<std::string>& right_cols,
    bool is_tsql) {
    std::string cond;
    const size_t n = (std::min)(left_cols.size(), right_cols.size());
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) cond += " AND ";
        cond += quote_sql_identifier(left, is_tsql) + "." +
                quote_sql_identifier(left_cols[i], is_tsql) + " = " +
                quote_sql_identifier(right, is_tsql) + "." +
                quote_sql_identifier(right_cols[i], is_tsql);
    }
    return cond;
}

std::string generate_new_alias(std::string_view table, const std::unordered_set<std::string>& taken, bool is_tsql) {
    std::string initials;
    bool next_initial = true;
    for (const unsigned char ch : table) {
        if (ch == '_') {
            next_initial = true;
            continue;
        }
        if (next_initial && std::isalpha(ch)) {
            initials.push_back(static_cast<char>(std::tolower(ch)));
            next_initial = false;
        }
    }
    if (initials.empty()) initials = "t";
    std::string alias = initials;
    for (int i = 2;; ++i) {
        std::string up = alias;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!taken.contains(alias) && !is_reserved(up, is_tsql)) {
            return alias;
        }
        alias = initials + std::to_string(i);
    }
}

void add_columns(
    std::vector<SqlCompletionItem>& items,
    const SqlCompletionCatalog& cat,
    size_t rel_index) {
    for (const SqlCatalogColumn& col : cat.relation_columns(rel_index)) {
        items.push_back(SqlCompletionItem{
            .text        = quote_sql_identifier(col.name, cat.is_tsql),
            .kind        = SqlCompletionKind::Column,
            .description = "column " + col.name + "(" + col.type + ", " + (col.nullable ? "null" : "not null") + ")",
            .filter      = col.name,
        });
    }
}

void add_schemas(std::vector<SqlCompletionItem>& items, const SqlCompletionCatalog& cat) {
    for (const std::string& sch : cat.schemas) {
        items.push_back(SqlCompletionItem{
            .text        = quote_sql_identifier(sch, cat.is_tsql),
            .kind        = SqlCompletionKind::Schema,
            .description = "schema " + cat.database + "." + sch,
            .filter      = sch,
        });
    }
}

void add_relations_in_schema(
    std::vector<SqlCompletionItem>& items,
    const SqlCompletionCatalog& cat,
    std::string_view schema) {
    for (const SqlCatalogRelation& r : cat.relations) {
        if (!eq_ident(r.schema, schema, cat.is_tsql)) continue;
        items.push_back(SqlCompletionItem{
            .text        = quote_sql_identifier(r.name, cat.is_tsql),
            .kind        = r.is_view ? SqlCompletionKind::View : SqlCompletionKind::Table,
            .description = std::string(r.is_view ? "view " : "table ") + cat.database + "." + r.schema + "." + r.name,
            .filter      = r.name,
        });
    }
}

void add_every_relation(std::vector<SqlCompletionItem>& items, const SqlCompletionCatalog& cat) {
    std::unordered_map<std::string, size_t> path_rank;
    for (size_t i = 0; i < cat.search_path.size(); ++i) {
        path_rank.try_emplace(fold_accents_lower(cat.search_path[i]), i);
    }
    std::unordered_map<std::string, std::string> reached;
    for (const SqlCatalogRelation& r : cat.relations) {
        const auto pit = path_rank.find(fold_accents_lower(r.schema));
        if (pit == path_rank.end()) continue;
        const std::string rkey = fold_accents_lower(r.name);
        auto rit = reached.find(rkey);
        if (rit == reached.end() || pit->second < path_rank[fold_accents_lower(rit->second)]) {
            reached[rkey] = r.schema;
        }
    }
    for (const SqlCatalogRelation& r : cat.relations) {
        if (r.schema == "pg_catalog" || r.schema == "information_schema" || r.schema == "sys") {
            continue;
        }
        const auto rit = reached.find(fold_accents_lower(r.name));
        const bool bare = (rit != reached.end() && eq_ident(rit->second, r.schema, cat.is_tsql));
        const std::string text = bare
            ? quote_sql_identifier(r.name, cat.is_tsql)
            : (quote_sql_identifier(r.schema, cat.is_tsql) + "." +
               quote_sql_identifier(r.name, cat.is_tsql));
        items.push_back(SqlCompletionItem{
            .text        = text,
            .kind        = r.is_view ? SqlCompletionKind::View : SqlCompletionKind::Table,
            .description = std::string(r.is_view ? "view " : "table ") + cat.database + "." + r.schema + "." + r.name,
            .filter      = r.name,
        });
    }
}

void add_functions(
    std::vector<SqlCompletionItem>& items,
    const SqlCompletionCatalog& cat,
    std::optional<std::string_view> only_schema,
    bool rows_only) {
    for (const SqlCatalogFunction& fn : cat.functions) {
        if (rows_only && !fn.returns_rows) continue;
        if (only_schema.has_value()) {
            if (!eq_ident(fn.schema, *only_schema, cat.is_tsql)) continue;
        } else {
            const bool in_path = std::any_of(cat.search_path.begin(), cat.search_path.end(), [&](const std::string& sp) {
                return eq_ident(sp, fn.schema, cat.is_tsql);
            });
            if (!in_path) continue;
            if (fn.schema == "pg_catalog" && fn.name.starts_with("pg_")) continue;
        }
        items.push_back(SqlCompletionItem{
            .text        = quote_sql_identifier(fn.name, cat.is_tsql),
            .kind        = SqlCompletionKind::Function,
            .description = "function " + cat.database + "." + fn.schema + "." + fn.name +
                           "(" + fn.arguments + ") RETURNS " + fn.result,
            .filter      = fn.name,
        });
    }
}

void add_joins(
    std::vector<SqlCompletionItem>& items,
    const SqlCompletionCatalog& cat,
    const std::vector<TableRef>& refs,
    size_t caret,
    std::optional<std::string_view> only_schema) {
    std::vector<const TableRef*> earlier;
    bool use_aliases = false;
    std::unordered_set<std::string> taken;
    for (const TableRef& r : refs) {
        if (r.start >= caret) continue;
        earlier.push_back(&r);
        if (r.alias.has_value()) use_aliases = true;
        taken.insert(r.alias.value_or(r.name));
    }

    auto add_one = [&](const TableRef& existing,
                       const std::string& schema,
                       const std::string& table,
                       const std::vector<std::string>& cols,
                       const std::vector<std::string>& existing_cols,
                       const std::string& key_name) {
        if (only_schema.has_value() && !eq_ident(schema, *only_schema, cat.is_tsql)) return;
        std::optional<std::string> alias;
        if (use_aliases || taken.contains(table)) {
            alias = generate_new_alias(table, taken, cat.is_tsql);
        }
        const bool in_path = std::any_of(cat.search_path.begin(), cat.search_path.end(), [&](const std::string& sp) {
            return eq_ident(sp, schema, cat.is_tsql);
        });
        const std::string name = (!only_schema.has_value() && !in_path)
            ? (quote_sql_identifier(schema, cat.is_tsql) + "." + quote_sql_identifier(table, cat.is_tsql))
            : quote_sql_identifier(table, cat.is_tsql);
        const std::string cond = make_join_condition(
            alias.value_or(table), cols, existing.alias.value_or(existing.name), existing_cols, cat.is_tsql);
        items.push_back(SqlCompletionItem{
            .text        = name + (alias.has_value() ? (" " + quote_sql_identifier(*alias, cat.is_tsql)) : "") + " ON " + cond,
            .kind        = SqlCompletionKind::Join,
            .description = "join " + cat.database + "." + schema + "." + table + " by foreign key " + key_name,
            .filter      = table,
        });
    };

    for (const TableRef* ref : earlier) {
        const auto resolved_idx = resolve_relation_index(cat, ref->schema, ref->name);
        if (!resolved_idx.has_value()) continue;
        const SqlCatalogRelation& resolved = cat.relations[*resolved_idx];
        for (const SqlCatalogForeignKey& fk : cat.foreign_keys) {
            if (eq_ident(fk.target_schema, resolved.schema, cat.is_tsql) && eq_ident(fk.target_table, resolved.name, cat.is_tsql)) {
                add_one(*ref, fk.schema, fk.table, fk.columns, fk.target_columns, fk.name);
            }
            if (eq_ident(fk.schema, resolved.schema, cat.is_tsql) && eq_ident(fk.table, resolved.name, cat.is_tsql)) {
                add_one(*ref, fk.target_schema, fk.target_table, fk.target_columns, fk.columns, fk.name);
            }
        }
    }
}

void add_join_conditions(
    std::vector<SqlCompletionItem>& items,
    const SqlCompletionCatalog& cat,
    const std::vector<TableRef>& refs,
    size_t caret) {
    std::vector<const TableRef*> earlier;
    for (const TableRef& r : refs) {
        if (r.start < caret) earlier.push_back(&r);
    }
    if (earlier.size() < 2) return;
    const TableRef& joined = *earlier.back();
    const auto joined_idx = resolve_relation_index(cat, joined.schema, joined.name);
    if (!joined_idx.has_value()) return;
    const SqlCatalogRelation& joined_rel = cat.relations[*joined_idx];

    for (size_t i = 0; i + 1 < earlier.size(); ++i) {
        const TableRef& other = *earlier[i];
        const auto other_idx = resolve_relation_index(cat, other.schema, other.name);
        if (!other_idx.has_value()) continue;
        const SqlCatalogRelation& other_rel = cat.relations[*other_idx];
        for (const SqlCatalogForeignKey& fk : cat.foreign_keys) {
            if (eq_ident(fk.schema, joined_rel.schema, cat.is_tsql) && eq_ident(fk.table, joined_rel.name, cat.is_tsql) &&
                eq_ident(fk.target_schema, other_rel.schema, cat.is_tsql) && eq_ident(fk.target_table, other_rel.name, cat.is_tsql)) {
                items.push_back(SqlCompletionItem{
                    .text        = make_join_condition(joined.alias.value_or(joined.name), fk.columns, other.alias.value_or(other.name), fk.target_columns, cat.is_tsql),
                    .kind        = SqlCompletionKind::Join,
                    .description = "condition from foreign key " + fk.name,
                    .filter      = {},
                });
            }
            if (eq_ident(fk.target_schema, joined_rel.schema, cat.is_tsql) && eq_ident(fk.target_table, joined_rel.name, cat.is_tsql) &&
                eq_ident(fk.schema, other_rel.schema, cat.is_tsql) && eq_ident(fk.table, other_rel.name, cat.is_tsql)) {
                items.push_back(SqlCompletionItem{
                    .text        = make_join_condition(joined.alias.value_or(joined.name), fk.target_columns, other.alias.value_or(other.name), fk.columns, cat.is_tsql),
                    .kind        = SqlCompletionKind::Join,
                    .description = "condition from foreign key " + fk.name,
                    .filter      = {},
                });
            }
        }
    }
}

} // namespace

size_t SqlCompletionCatalog::find_or_add_relation(std::string_view schema, std::string_view table, bool is_view) {
    if (hint_relation_index < relations.size()) {
        const auto& r0 = relations[hint_relation_index];
        if (r0.schema == schema && r0.name == table) {
            return hint_relation_index;
        }
    }
    if (hint_relation_index + 1u < relations.size()) {
        const auto& r1 = relations[hint_relation_index + 1u];
        if (r1.schema == schema && r1.name == table) {
            return ++hint_relation_index;
        }
    }
    for (size_t i = 0; i < relations.size(); ++i) {
        if (relations[i].schema == schema && relations[i].name == table) {
            hint_relation_index = static_cast<uint32_t>(i);
            return i;
        }
    }
    const size_t new_idx = relations.size();
    relations.push_back(SqlCatalogRelation{
        .schema       = std::string(schema),
        .name         = std::string(table),
        .is_view      = is_view,
        .first_column = static_cast<uint32_t>(all_columns.size()),
        .column_count = 0u,
    });
    hint_relation_index = static_cast<uint32_t>(new_idx);
    return new_idx;
}

void SqlCompletionCatalog::append_column(std::string_view schema, std::string_view table, SqlCatalogColumn col) {
    const size_t rel_idx = find_or_add_relation(schema, table, false);
    SqlCatalogRelation& rel = relations[rel_idx];
    if (rel.column_count == 0) {
        rel.first_column = static_cast<uint32_t>(all_columns.size());
        all_columns.push_back(std::move(col));
        rel.column_count = 1u;
    } else if (static_cast<size_t>(rel.first_column) + rel.column_count == all_columns.size()) {
        all_columns.push_back(std::move(col));
        ++rel.column_count;
    } else {
        const uint32_t insert_at = rel.first_column + rel.column_count;
        all_columns.insert(all_columns.begin() + static_cast<ptrdiff_t>(insert_at), std::move(col));
        ++rel.column_count;
        for (size_t i = 0; i < relations.size(); ++i) {
            if (i != rel_idx && relations[i].column_count > 0 && relations[i].first_column >= insert_at) {
                ++relations[i].first_column;
            }
        }
    }
}

void SqlCompletionCatalog::set_relation_columns(
    std::string_view schema,
    std::string_view table,
    std::initializer_list<SqlCatalogColumn> cols) {
    for (const SqlCatalogColumn& c : cols) {
        append_column(schema, table, c);
    }
}

std::string quote_sql_identifier(std::string_view name, bool is_tsql) {
    if (name.empty()) {
        return is_tsql ? "[]" : "\"\"";
    }
    bool simple = false;
    if (is_tsql) {
        simple = std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_';
        for (size_t i = 1; simple && i < name.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            simple = std::isalnum(c) || c == '_' || c == '$';
        }
    } else {
        simple = std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_';
        for (size_t i = 1; simple && i < name.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            simple = std::islower(c) || std::isdigit(c) || c == '_' || c == '$';
        }
    }
    std::string up(name);
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (simple && !is_reserved(up, is_tsql)) {
        return std::string(name);
    }
    if (is_tsql) {
        std::string out = "[";
        for (char c : name) {
            if (c == ']') out += "]]";
            else out.push_back(c);
        }
        out.push_back(']');
        return out;
    }
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::optional<SqlCompletionResult> complete_sql(
    std::string_view text,
    size_t caret_utf8,
    const SqlCompletionCatalog& catalog) {
    if (caret_utf8 > text.size() || text.size() > kMaxSqlCompletionTextBytes) {
        return std::nullopt;
    }
    const auto tokens_opt = tokenize_for_completion(text, caret_utf8, catalog.is_tsql);
    if (!tokens_opt.has_value()) {
        return std::nullopt;
    }
    const std::vector<Tok>& tokens = *tokens_opt;

    ptrdiff_t current = -1;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].is_name() && tokens[i].start < caret_utf8 && caret_utf8 <= tokens[i].end) {
            current = static_cast<ptrdiff_t>(i);
            break;
        }
    }

    std::string prefix;
    size_t replace_start = caret_utf8;
    ptrdiff_t before = -1;
    if (current >= 0) {
        const Tok& word = tokens[static_cast<size_t>(current)];
        replace_start = word.start;
        if (word.kind == TokKind::Quoted) {
            const size_t pstart = (std::min)(word.start + 1, caret_utf8);
            prefix.assign(text.substr(pstart, caret_utf8 - pstart));
            if (!prefix.empty() && (prefix.back() == '"' || (catalog.is_tsql && prefix.back() == ']'))) {
                prefix.pop_back();
            }
        } else {
            prefix.assign(text.substr(word.start, caret_utf8 - word.start));
        }
        before = current - 1;
    } else {
        for (ptrdiff_t i = static_cast<ptrdiff_t>(tokens.size()) - 1; i >= 0; --i) {
            if (tokens[static_cast<size_t>(i)].end <= caret_utf8) {
                before = i;
                break;
            }
        }
    }

    // Extract current statement bounded by ';'
    ptrdiff_t first = 0;
    for (ptrdiff_t i = before; i >= 0; --i) {
        if (tokens[static_cast<size_t>(i)].kind == TokKind::Symbol &&
            tokens[static_cast<size_t>(i)].text == ";" &&
            tokens[static_cast<size_t>(i)].end <= replace_start) {
            first = i + 1;
            break;
        }
    }
    size_t last = tokens.size();
    for (size_t i = static_cast<size_t>(first); i < tokens.size(); ++i) {
        if (tokens[i].kind == TokKind::Symbol && tokens[i].text == ";" && tokens[i].start >= caret_utf8) {
            last = i;
            break;
        }
    }

    std::vector<Tok> statement(tokens.begin() + first, tokens.begin() + static_cast<ptrdiff_t>(last));
    const std::vector<TableRef> references = collect_references(statement, replace_start, catalog.is_tsql);

    std::vector<SqlCompletionItem> items;
    if (before >= first &&
        tokens[static_cast<size_t>(before)].kind == TokKind::Symbol &&
        tokens[static_cast<size_t>(before)].text == ".") {
        if (before - 1 < first || !tokens[static_cast<size_t>(before - 1)].is_name()) {
            return std::nullopt;
        }
        const std::string qualifier = tokens[static_cast<size_t>(before - 1)].name_norm(catalog.is_tsql);
        const Place place = place_at(tokens, before - 2, first);

        if (place == Place::Rows || place == Place::Relation) {
            if (const auto sch = find_schema_in_catalog(catalog, qualifier)) {
                add_relations_in_schema(items, catalog, *sch);
                if (place == Place::Rows) add_functions(items, catalog, *sch, true);
                if (before >= 2 && tokens[static_cast<size_t>(before - 2)].upper() == "JOIN") {
                    add_joins(items, catalog, references, replace_start, *sch);
                }
            }
        } else if (before - 3 >= first &&
                   tokens[static_cast<size_t>(before - 2)].kind == TokKind::Symbol &&
                   tokens[static_cast<size_t>(before - 2)].text == "." &&
                   tokens[static_cast<size_t>(before - 3)].is_name()) {
            const std::string sch_qual = tokens[static_cast<size_t>(before - 3)].name_norm(catalog.is_tsql);
            if (const auto ridx = resolve_relation_index(catalog, sch_qual, qualifier)) {
                add_columns(items, catalog, *ridx);
            }
        } else {
            bool matched_alias = false;
            for (const TableRef& ref : references) {
                if ((ref.alias.has_value() && eq_ident(*ref.alias, qualifier, catalog.is_tsql)) ||
                    (!ref.alias.has_value() && eq_ident(ref.name, qualifier, catalog.is_tsql))) {
                    matched_alias = true;
                    if (const auto ridx = resolve_relation_index(catalog, ref.schema, ref.name)) {
                        add_columns(items, catalog, *ridx);
                    }
                }
            }
            if (!matched_alias) {
                if (const auto visible = resolve_relation_index(catalog, std::nullopt, qualifier)) {
                    add_columns(items, catalog, *visible);
                }
            }
            if (const auto sch = find_schema_in_catalog(catalog, qualifier)) {
                add_relations_in_schema(items, catalog, *sch);
                add_functions(items, catalog, *sch, false);
            }
        }
    } else {
        const Place place = place_at(tokens, before, first);
        if (place == Place::Rows || place == Place::Relation) {
            add_schemas(items, catalog);
            add_every_relation(items, catalog);
            if (place == Place::Rows) add_functions(items, catalog, std::nullopt, true);
            if (before >= first && tokens[static_cast<size_t>(before)].upper() == "JOIN") {
                add_joins(items, catalog, references, replace_start, std::nullopt);
            }
        } else if (place == Place::Start) {
            const std::span<const std::string_view> starts = catalog.is_tsql
                ? std::span<const std::string_view>(kTsqlStatements)
                : std::span<const std::string_view>(kPgStatements);
            for (const std::string_view kw : starts) {
                std::string kw_str(kw);
                items.push_back(SqlCompletionItem{kw_str, SqlCompletionKind::Keyword, "keyword", kw_str});
            }
        } else {
            if (before >= first && tokens[static_cast<size_t>(before)].upper() == "ON") {
                add_join_conditions(items, catalog, references, replace_start);
            }
            for (const TableRef& ref : references) {
                const auto ridx = resolve_relation_index(catalog, ref.schema, ref.name);
                const SqlCatalogRelation* rel = ridx.has_value() ? &catalog.relations[*ridx] : nullptr;
                if (ridx.has_value()) {
                    add_columns(items, catalog, *ridx);
                }
                const bool is_view = (rel != nullptr && rel->is_view);
                const std::string target = (rel != nullptr)
                    ? (catalog.database + "." + rel->schema + "." + rel->name)
                    : ref.name;
                const std::string desc = ref.alias.has_value()
                    ? ("alias " + *ref.alias + " of " + target)
                    : (std::string(is_view ? "view " : "table ") + target);
                const std::string label = ref.alias.value_or(ref.name);
                items.push_back(SqlCompletionItem{
                    .text        = quote_sql_identifier(label, catalog.is_tsql),
                    .kind        = is_view ? SqlCompletionKind::View : SqlCompletionKind::Table,
                    .description = desc,
                    .filter      = label,
                });
            }
            add_functions(items, catalog, std::nullopt, false);
            const std::span<const std::string_view> kw_set = catalog.is_tsql
                ? std::span<const std::string_view>(kTsqlReserved)
                : std::span<const std::string_view>(kPgReserved);
            for (const std::string_view kw : kw_set) {
                std::string kw_str(kw);
                items.push_back(SqlCompletionItem{kw_str, SqlCompletionKind::Keyword, "keyword", kw_str});
            }
        }
    }

    // Deduplicate by (text, kind) and sort deterministically (Joins first, then folded name)
    std::set<std::pair<std::string, uint8_t>> seen;
    std::vector<SqlCompletionItem> deduped;
    deduped.reserve(items.size());
    for (auto& it : items) {
        if (seen.emplace(it.text, static_cast<uint8_t>(it.kind)).second) {
            deduped.push_back(std::move(it));
        }
    }

    std::sort(deduped.begin(), deduped.end(), [](const SqlCompletionItem& a, const SqlCompletionItem& b) {
        const int ja = (a.kind == SqlCompletionKind::Join) ? 0 : 1;
        const int jb = (b.kind == SqlCompletionKind::Join) ? 0 : 1;
        if (ja != jb) return ja < jb;
        const std::string fa = fold_accents_lower(a.filter.empty() ? a.text : a.filter);
        const std::string fb = fold_accents_lower(b.filter.empty() ? b.text : b.filter);
        if (fa != fb) return fa < fb;
        if (a.text.size() != b.text.size()) return a.text.size() < b.text.size();
        return a.text < b.text;
    });

    return SqlCompletionResult{
        .replace_start = replace_start,
        .prefix        = std::move(prefix),
        .items         = std::move(deduped),
    };
}

std::vector<SqlCompletionItem> filter_sql_completion_items(
    const std::vector<SqlCompletionItem>& items,
    std::string_view prefix) {
    const std::string needle = fold_accents_lower(prefix);
    if (needle.empty()) {
        return items;
    }

    struct Scored {
        const SqlCompletionItem* item = nullptr;
        int                      tier = 2; // 0 = exact/prefix on filter, 1 = prefix on text, 2 = contains
    };
    std::vector<Scored> matches;
    matches.reserve(items.size());
    for (const SqlCompletionItem& it : items) {
        const std::string key = fold_accents_lower(it.filter.empty() ? it.text : it.filter);
        const std::string txt = fold_accents_lower(it.text);
        if (key.starts_with(needle)) {
            matches.push_back(Scored{&it, 0});
        } else if (txt.starts_with(needle)) {
            matches.push_back(Scored{&it, 1});
        } else if (key.find(needle) != std::string::npos || txt.find(needle) != std::string::npos) {
            matches.push_back(Scored{&it, 2});
        }
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Scored& a, const Scored& b) {
        const int ja = (a.item->kind == SqlCompletionKind::Join) ? 0 : 1;
        const int jb = (b.item->kind == SqlCompletionKind::Join) ? 0 : 1;
        if (a.tier != b.tier) return a.tier < b.tier;
        if (ja != jb) return ja < jb;
        return a.item->text.size() < b.item->text.size();
    });

    std::vector<SqlCompletionItem> out;
    out.reserve(matches.size());
    for (const Scored& m : matches) {
        out.push_back(*m.item);
    }
    return out;
}

} // namespace capybaradb::core
