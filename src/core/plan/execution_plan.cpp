#include "core/plan/execution_plan.hpp"

#include "core/config/connection_profile.hpp"
#include "core/config/file_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace capybaradb::core {
namespace {

constexpr size_t kMaxObjectName = 34u;
constexpr size_t kMaxPropValue  = 4096u;

std::string shorten(std::string s, size_t max_len = kMaxPropValue) {
    if (s.size() <= max_len) {
        return s;
    }
    size_t n = max_len;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) {
        --n;
    }
    s.resize(n);
    s += " [truncated]";
    return s;
}

std::string shorten_object(std::string s) {
    if (s.size() <= kMaxObjectName) {
        return s;
    }
    size_t n = kMaxObjectName - 1;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) {
        --n;
    }
    s.resize(n);
    s += "\xE2\x80\xA6";
    return s;
}

std::string flatten_sql(std::string_view sql) {
    std::string out;
    out.reserve(sql.size());
    bool space = false;
    for (const unsigned char c : sql) {
        if (std::isspace(c) != 0) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string fmt_double(double v, int max_decimals = 3) {
    if (!std::isfinite(v)) return "0";
    char buf[64]{};
    if (max_decimals == 0) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.*f", max_decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

std::string fmt_fixed(double v, int decimals) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::isfinite(v) ? v : 0.0);
    return buf;
}

std::optional<double> parse_double(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    if (sv.empty()) return std::nullopt;
    double val = 0.0;
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (res.ec == std::errc{} && res.ptr == sv.data() + sv.size() && std::isfinite(val)) {
        return val;
    }
    return std::nullopt;
}

std::string statement_keyword(std::string_view sql) {
    size_t i = 0;
    while (i < sql.size()) {
        if (std::isspace(static_cast<unsigned char>(sql[i]))) {
            ++i;
        } else if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
            const size_t nl = sql.find('\n', i + 2);
            if (nl == std::string_view::npos) return "STATEMENT";
            i = nl + 1;
        } else if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
            const size_t end = sql.find("*/", i + 2);
            if (end == std::string_view::npos) return "STATEMENT";
            i = end + 2;
        } else {
            break;
        }
    }
    const size_t start = i;
    while (i < sql.size() && std::isalpha(static_cast<unsigned char>(sql[i]))) ++i;
    std::string word;
    for (size_t k = start; k < i; ++k) {
        word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(sql[k]))));
    }
    if (word == "SELECT" || word == "INSERT" || word == "UPDATE" || word == "DELETE" ||
        word == "MERGE" || word == "WITH" || word == "VALUES" || word == "TABLE" ||
        word == "EXECUTE" || word == "EXEC" || word == "CREATE") {
        return word;
    }
    return "STATEMENT";
}

bool icontains_ascii(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    const auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

int icompare_ascii(std::string_view a, std::string_view b) noexcept {
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    if (a.size() != b.size()) return (a.size() < b.size()) ? -1 : 1;
    return 0;
}

std::string quote_pg_ident(std::string_view name) {
    bool simple = !name.empty() && (std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_');
    for (size_t i = 1; simple && i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        simple = std::islower(c) || std::isdigit(c) || c == '_' || c == '$';
    }
    if (simple) return std::string(name);
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string quote_tsql_ident(std::string_view name) {
    if (name.size() >= 2 && name.front() == '[' && name.back() == ']') {
        return std::string(name);
    }
    std::string out = "[";
    for (char c : name) {
        if (c == ']') out += "]]";
        else out.push_back(c);
    }
    out.push_back(']');
    return out;
}

// ===========================================================================
// PostgreSQL EXPLAIN (FORMAT JSON) Parser
// ===========================================================================

struct PgRawNode {
    std::vector<std::pair<std::string, std::string>> props;
    std::vector<PgRawNode>                           children;

    [[nodiscard]] std::optional<std::string_view> prop(std::string_view key) const noexcept {
        for (const auto& [k, v] : props) {
            if (k == key) return v;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> num(std::string_view key) const noexcept {
        if (const auto v = prop(key)) return parse_double(*v);
        return std::nullopt;
    }
};

std::expected<PgRawNode, std::string> read_pg_json_node(
    const nlohmann::json& j,
    size_t depth,
    size_t& count) {
    if (depth > kMaxExecutionPlanDepth || ++count > kMaxExecutionPlanNodes) {
        return std::unexpected(std::string{"Plan exceeds 4096 nodes or 64 levels; no partial tree is displayed."});
    }
    if (!j.is_object() || !j.contains("Node Type") || !j["Node Type"].is_string()) {
        return std::unexpected(std::string{"Plan node has no 'Node Type'."});
    }
    PgRawNode node;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "Plans") continue;
        std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        node.props.emplace_back(it.key(), shorten(std::move(val)));
    }
    if (j.contains("Plans")) {
        if (!j["Plans"].is_array()) {
            return std::unexpected(std::string{"Invalid child 'Plans' list."});
        }
        for (const auto& child : j["Plans"]) {
            auto ch = read_pg_json_node(child, depth + 1, count);
            if (!ch) return std::unexpected(ch.error());
            node.children.push_back(std::move(*ch));
        }
    }
    return node;
}

double pg_self_cost(const PgRawNode& n) noexcept {
    double child_sum = 0.0;
    for (const auto& c : n.children) {
        child_sum += c.num("Total Cost").value_or(0.0);
    }
    return (std::max)(0.0, n.num("Total Cost").value_or(0.0) - child_sum);
}

double pg_total_self_cost(const PgRawNode& n) noexcept {
    double s = pg_self_cost(n);
    for (const auto& c : n.children) {
        s += pg_total_self_cost(c);
    }
    return s;
}

ExecutionPlanIcon pg_icon_for(std::string_view type) noexcept {
    if (type == "Seq Scan" || type == "Sample Scan" || type == "Tid Scan" ||
        type == "Tid Range Scan" || type == "Foreign Scan" || type == "Custom Scan")
        return ExecutionPlanIcon::TableScan;
    if (type == "Index Scan" || type == "Index Only Scan" || type == "Bitmap Index Scan")
        return ExecutionPlanIcon::IndexScan;
    if (type == "Bitmap Heap Scan" || type == "BitmapAnd" || type == "BitmapOr")
        return ExecutionPlanIcon::BitmapScan;
    if (type == "Nested Loop" || type == "Hash Join" || type == "Merge Join")
        return ExecutionPlanIcon::Join;
    if (type == "Sort" || type == "Incremental Sort")
        return ExecutionPlanIcon::Sort;
    if (type == "Aggregate" || type == "Group" || type == "WindowAgg" || type == "Unique" || type == "SetOp")
        return ExecutionPlanIcon::Aggregate;
    if (type == "Limit") return ExecutionPlanIcon::Limit;
    if (type == "Materialize" || type == "Memoize" || type == "Hash")
        return ExecutionPlanIcon::Materialize;
    if (type == "Append" || type == "Merge Append" || type == "Recursive Union")
        return ExecutionPlanIcon::Append;
    if (type == "Gather" || type == "Gather Merge")
        return ExecutionPlanIcon::Gather;
    if (type == "ModifyTable" || type == "LockRows")
        return ExecutionPlanIcon::Modify;
    if (type == "Subquery Scan" || type == "CTE Scan" || type == "Function Scan" ||
        type == "Values Scan" || type == "Table Function Scan" || type == "WorkTable Scan")
        return ExecutionPlanIcon::Subquery;
    return ExecutionPlanIcon::Other;
}

std::string pg_describe(std::string_view type) {
    if (type == "Seq Scan") return "Reads every row of the table in physical order.";
    if (type == "Index Scan") return "Finds rows through an index and fetches them from the table.";
    if (type == "Index Only Scan") return "Answers from the index alone, visiting the table only for visibility checks.";
    if (type == "Bitmap Index Scan") return "Marks table pages holding matching rows using an index.";
    if (type == "Bitmap Heap Scan") return "Reads table pages marked by bitmap index scans.";
    if (type == "Nested Loop") return "For each row of the top input, scans the bottom input for matches.";
    if (type == "Hash Join") return "Builds a hash table from the bottom input and probes it with the top input.";
    if (type == "Merge Join") return "Joins two inputs already sorted on the join keys.";
    if (type == "Sort" || type == "Incremental Sort") return "Orders input rows by the sort keys.";
    if (type == "Aggregate") return "Computes aggregate functions across groups of rows.";
    if (type == "Limit") return "Returns at most the requested number of rows.";
    if (type == "Gather" || type == "Gather Merge") return "Collects rows produced by parallel worker processes.";
    return {};
}

std::string readable_json_list(std::string_view val) {
    if (!val.starts_with('[')) return std::string(val);
    const auto parsed = nlohmann::json::parse(val, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return std::string(val);
    std::string out;
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (i > 0) out += ", ";
        out += parsed[i].is_string() ? parsed[i].get<std::string>() : parsed[i].dump();
    }
    return out;
}

std::vector<std::string> extract_pg_filter_columns(std::string_view filter, std::string_view relation, std::string_view alias) {
    std::vector<std::string> cols;
    // Heuristic scan for simple column = / < / > / <= / >= literal comparisons
    size_t i = 0;
    while (i < filter.size()) {
        if (std::isalpha(static_cast<unsigned char>(filter[i])) || filter[i] == '_') {
            const size_t start = i;
            while (i < filter.size() && (std::isalnum(static_cast<unsigned char>(filter[i])) || filter[i] == '_' || filter[i] == '.')) {
                ++i;
            }
            std::string_view tok = filter.substr(start, i - start);
            size_t j = i;
            while (j < filter.size() && filter[j] == ' ') ++j;
            if (j < filter.size() && (filter[j] == '=' || filter[j] == '<' || filter[j] == '>')) {
                std::string col_name;
                const size_t dot = tok.find('.');
                if (dot != std::string_view::npos) {
                    const std::string_view qual = tok.substr(0, dot);
                    if (qual == alias || qual == relation) {
                        col_name = std::string(tok.substr(dot + 1));
                    }
                } else {
                    std::string up(tok);
                    for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    if (up != "AND" && up != "OR" && up != "NOT" && up != "NULL" && up != "TRUE" && up != "FALSE") {
                        col_name = std::string(tok);
                    }
                }
                if (!col_name.empty() && std::find(cols.begin(), cols.end(), col_name) == cols.end()) {
                    cols.push_back(std::move(col_name));
                }
            }
        } else if (filter[i] == '\'') {
            ++i;
            while (i < filter.size()) {
                if (filter[i] == '\'') {
                    if (i + 1 < filter.size() && filter[i + 1] == '\'') i += 2;
                    else { ++i; break; }
                } else {
                    ++i;
                }
            }
        } else {
            ++i;
        }
    }
    return cols;
}

ExecutionPlanOperator build_pg_operator(
    const PgRawNode& n,
    double total_cost,
    const ExecutionPlanWarningOptions& warn_opts,
    std::vector<PlanIndexSuggestion>& missing_indexes_out) {
    const std::string type = std::string(n.prop("Node Type").value_or("Operator"));
    const double self = pg_self_cost(n);
    const double pct  = (total_cost > 0.0) ? (100.0 * self / total_cost) : 0.0;
    const std::optional<double> loops       = n.num("Actual Loops");
    const std::optional<double> raw_actual  = n.num("Actual Rows");
    const std::optional<double> raw_time_ms = n.num("Actual Total Time");

    std::optional<double> actual_rows;
    if (loops.has_value() && raw_actual.has_value()) {
        actual_rows = (*raw_actual) * (*loops);
    }
    std::optional<double> seconds;
    if (loops.has_value() && raw_time_ms.has_value()) {
        seconds = (*raw_time_ms) * (*loops) / 1000.0;
    }
    const double est_rows = n.num("Plan Rows").value_or(0.0) * (std::max)(loops.value_or(1.0), 1.0);

    std::string subtitle;
    if (const auto jt = n.prop("Join Type")) {
        subtitle = std::string(*jt) + (jt->ends_with("Join") ? "" : " Join");
    } else if (type == "Aggregate") {
        if (const auto st = n.prop("Strategy")) subtitle = std::string(*st);
    } else if (type == "ModifyTable") {
        if (const auto op = n.prop("Operation")) subtitle = std::string(*op);
    } else if (type == "Sort") {
        if (const auto sm = n.prop("Sort Method")) subtitle = std::string(*sm);
    }

    std::string obj;
    if (const auto rel = n.prop("Relation Name")) obj = std::string(*rel);
    else if (const auto cte = n.prop("CTE Name")) obj = std::string(*cte);
    else if (const auto fn = n.prop("Function Name")) obj = std::string(*fn);
    if (!obj.empty()) {
        if (const auto sch = n.prop("Schema")) obj = std::string(*sch) + "." + obj;
    }
    if (const auto idx = n.prop("Index Name")) {
        obj = obj.empty() ? std::string(*idx) : (obj + "." + std::string(*idx));
    }
    obj = shorten_object(std::move(obj));

    ExecutionPlanOperator op;
    op.title          = type;
    op.subtitle       = subtitle;
    op.object_name    = obj;
    op.icon           = pg_icon_for(type);
    op.cost_percent   = pct;
    op.seconds        = seconds;
    op.actual_rows    = actual_rows;
    op.estimated_rows = est_rows;
    op.description    = pg_describe(type);
    op.properties     = n.props;

    auto add_sum = [&](std::string label, std::string val) {
        if (!val.empty()) op.summary.emplace_back(std::move(label), std::move(val));
    };
    add_sum("Physical Operation", type);
    add_sum("Logical Operation", subtitle);
    if (actual_rows) add_sum("Actual Number of Rows for All Executions", fmt_double(*actual_rows));
    add_sum("Estimated Number of Rows for All Executions", fmt_double(est_rows));
    if (const auto pr = n.prop("Plan Rows")) add_sum("Estimated Number of Rows Per Execution", std::string(*pr));
    if (loops) add_sum("Number of Executions", fmt_double(*loops));
    if (seconds) add_sum("Actual Time for All Executions", fmt_fixed(*seconds, 3) + " s");
    add_sum("Estimated Operator Cost", fmt_double(self, 2) + " (" + fmt_double(pct, 0) + "%)");
    if (const auto sc = n.prop("Startup Cost")) add_sum("Estimated Startup Cost", std::string(*sc));
    if (const auto tc = n.prop("Total Cost")) add_sum("Estimated Subtree Cost", std::string(*tc));
    if (const auto pw = n.prop("Plan Width")) add_sum("Estimated Row Size", std::string(*pw) + " B");
    if (const auto sh = n.prop("Shared Hit Blocks")) add_sum("Shared Blocks Hit", std::string(*sh));
    if (const auto sr = n.prop("Shared Read Blocks")) add_sum("Shared Blocks Read", std::string(*sr));
    if (const auto wl = n.prop("Workers Launched")) add_sum("Workers Launched", std::string(*wl));

    for (const char* detail_key : {"Output", "Index Cond", "Recheck Cond", "Hash Cond", "Merge Cond", "Join Filter", "Filter", "Sort Key", "Group Key"}) {
        if (const auto dv = n.prop(detail_key)) {
            op.details.emplace_back(detail_key, readable_json_list(*dv));
        }
    }

    // Discarded rows warning detection (CAPYBARADB-040)
    if (loops.has_value() && actual_rows.has_value()) {
        for (const char* counter : {"Rows Removed by Filter", "Rows Removed by Join Filter", "Rows Removed by Index Recheck"}) {
            if (const auto per_loop = n.num(counter)) {
                const double removed = (*per_loop) * (*loops);
                if (removed > 0.0 &&
                    removed >= static_cast<double>(warn_opts.removed_rows_minimum) &&
                    removed >= static_cast<double>(warn_opts.removed_rows_ratio) * (*actual_rows)) {
                    std::string warn = std::string(counter) + ": " + fmt_double(removed) +
                                       ", against " + fmt_double(*actual_rows) +
                                       (*actual_rows == 1.0 ? " row returned" : " rows returned");
                    if (*actual_rows > 0.0) {
                        warn += " (" + fmt_double(removed / (*actual_rows), 0) + " times).";
                    } else {
                        warn += ".";
                    }
                    op.warnings.push_back(std::move(warn));

                    // Heuristic Missing Index Suggestion on filtered scans (CAPYBARADB-041)
                    if (std::string_view(counter) == "Rows Removed by Filter" &&
                        (type == "Seq Scan" || type == "Bitmap Heap Scan" || type == "Index Scan")) {
                        if (const auto rel = n.prop("Relation Name"); rel && n.prop("Filter")) {
                            const std::string sch = std::string(n.prop("Schema").value_or("public"));
                            const std::string ali = std::string(n.prop("Alias").value_or(*rel));
                            auto cols = extract_pg_filter_columns(*n.prop("Filter"), *rel, ali);
                            if (!cols.empty()) {
                                PlanIndexSuggestion sug;
                                sug.schema      = sch;
                                sug.table       = std::string(*rel);
                                sug.key_columns = cols;
                                std::string key_list;
                                for (size_t ci = 0; ci < cols.size(); ++ci) {
                                    if (ci > 0) key_list += ", ";
                                    key_list += quote_pg_ident(cols[ci]);
                                }
                                const std::string qtbl = quote_pg_ident(sch) + "." + quote_pg_ident(*rel);
                                sug.header_line = "Missing Index (heuristic, not verified): CREATE INDEX ON " + qtbl + " (" + key_list + ")";
                                sug.commented_script =
                                    "/* Suggested index based on plan filter (" + fmt_double(removed) + " rows removed by filter).\n"
                                    "   Review existing indexes and selectivity before executing.\n"
                                    "   CREATE INDEX idx_" + std::string(*rel) + "_" + cols[0] +
                                    " ON " + qtbl + " (" + key_list + ");\n*/\n";
                                missing_indexes_out.push_back(std::move(sug));
                            }
                        }
                    }
                }
            }
        }
    }

    if (!op.warnings.empty()) {
        std::string joined;
        for (size_t i = 0; i < op.warnings.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += op.warnings[i];
        }
        op.properties.emplace_back("Warnings", std::move(joined));
    }

    op.children.reserve(n.children.size());
    for (const auto& ch : n.children) {
        op.children.push_back(build_pg_operator(ch, total_cost, warn_opts, missing_indexes_out));
    }
    return op;
}

// ===========================================================================
// Lightweight Pure C++23 XML DOM for SQL Server ShowPlan XML
// ===========================================================================

struct XmlElement {
    std::string                                       name; // Local tag name (namespace prefix stripped)
    std::vector<std::pair<std::string, std::string>>  attrs;
    std::string                                       text;
    std::vector<XmlElement>                           children;

    [[nodiscard]] std::optional<std::string_view> attr(std::string_view key) const noexcept {
        for (const auto& [k, v] : attrs) {
            if (k == key) return v;
        }
        return std::nullopt;
    }

    [[nodiscard]] const XmlElement* first_child(std::string_view tag) const noexcept {
        for (const auto& ch : children) {
            if (ch.name == tag) return &ch;
        }
        return nullptr;
    }
};

std::string xml_unescape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] != '&') {
            out.push_back(sv[i]);
            continue;
        }
        const size_t semi = sv.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        const std::string_view ent = sv.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() >= 2 && ent[0] == '#') {
            uint32_t cp = 0;
            const bool hex = (ent[1] == 'x' || ent[1] == 'X');
            const std::string_view digits = hex ? ent.substr(2) : ent.substr(1);
            const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            if (r.ec == std::errc{} && cp > 0 && cp <= 0x10FFFFu) {
                if (cp < 0x80u) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800u) {
                    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else if (cp < 0x10000u) {
                    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else {
                    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                }
            }
        } else {
            out.append(sv.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

std::string strip_xml_prefix(std::string_view tag) {
    const size_t colon = tag.find(':');
    return std::string((colon == std::string_view::npos) ? tag : tag.substr(colon + 1));
}

class MiniXmlParser final {
public:
    explicit MiniXmlParser(std::string_view src) : src_(src) {}

    [[nodiscard]] std::expected<XmlElement, std::string> parse_root() {
        skip_bom_and_prolog();
        if (pos_ >= src_.size() || src_[pos_] != '<') {
            return std::unexpected(std::string{"Expected XML root element in SQL Server ShowPlan."});
        }
        return parse_element(0);
    }

private:
    void skip_ws() noexcept {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    void skip_bom_and_prolog() {
        if (src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ = 3;
        }
        for (;;) {
            skip_ws();
            if (pos_ + 2 <= src_.size() && src_.substr(pos_, 2) == "<?") {
                const size_t end = src_.find("?>", pos_ + 2);
                pos_ = (end == std::string_view::npos) ? src_.size() : (end + 2);
            } else if (pos_ + 4 <= src_.size() && src_.substr(pos_, 4) == "<!--") {
                const size_t end = src_.find("-->", pos_ + 4);
                pos_ = (end == std::string_view::npos) ? src_.size() : (end + 3);
            } else if (pos_ + 2 <= src_.size() && src_.substr(pos_, 2) == "<!") {
                const size_t end = src_.find('>', pos_ + 2);
                pos_ = (end == std::string_view::npos) ? src_.size() : (end + 1);
            } else {
                break;
            }
        }
    }

    std::string read_name() {
        const size_t start = pos_;
        while (pos_ < src_.size()) {
            const unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.') ++pos_;
            else break;
        }
        return std::string(src_.substr(start, pos_ - start));
    }

    std::expected<XmlElement, std::string> parse_element(size_t depth) {
        if (depth > 128u || ++node_count_ > 65536u) {
            return std::unexpected(std::string{"XML execution plan exceeds maximum depth or element count."});
        }
        if (pos_ >= src_.size() || src_[pos_] != '<') {
            return std::unexpected(std::string{"Malformed XML element."});
        }
        ++pos_; // '<'
        const std::string raw_name = read_name();
        if (raw_name.empty()) {
            return std::unexpected(std::string{"Empty XML element name."});
        }
        XmlElement el;
        el.name = strip_xml_prefix(raw_name);

        // Parse attributes
        for (;;) {
            skip_ws();
            if (pos_ >= src_.size()) {
                return std::unexpected(std::string{"Unterminated XML start tag."});
            }
            if (src_[pos_] == '>') {
                ++pos_;
                break;
            }
            if (pos_ + 1 < src_.size() && src_[pos_] == '/' && src_[pos_ + 1] == '>') {
                pos_ += 2;
                return el;
            }
            const std::string attr_name = strip_xml_prefix(read_name());
            skip_ws();
            if (attr_name.empty() || pos_ >= src_.size() || src_[pos_] != '=') {
                return std::unexpected(std::string{"Malformed XML attribute."});
            }
            ++pos_;
            skip_ws();
            if (pos_ >= src_.size() || (src_[pos_] != '"' && src_[pos_] != '\'')) {
                return std::unexpected(std::string{"Expected quoted XML attribute value."});
            }
            const char q = src_[pos_++];
            const size_t vstart = pos_;
            while (pos_ < src_.size() && src_[pos_] != q) ++pos_;
            if (pos_ >= src_.size()) {
                return std::unexpected(std::string{"Unterminated XML attribute value."});
            }
            el.attrs.emplace_back(attr_name, xml_unescape(src_.substr(vstart, pos_ - vstart)));
            ++pos_;
        }

        // Parse children / text until </...>
        for (;;) {
            if (pos_ >= src_.size()) {
                return std::unexpected(std::string{"Unexpected end of XML document."});
            }
            if (src_[pos_] == '<') {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                    pos_ += 2;
                    (void)read_name();
                    const size_t close = src_.find('>', pos_);
                    pos_ = (close == std::string_view::npos) ? src_.size() : (close + 1);
                    return el;
                }
                if (pos_ + 4 <= src_.size() && src_.substr(pos_, 4) == "<!--") {
                    const size_t end = src_.find("-->", pos_ + 4);
                    pos_ = (end == std::string_view::npos) ? src_.size() : (end + 3);
                    continue;
                }
                if (pos_ + 9 <= src_.size() && src_.substr(pos_, 9) == "<![CDATA[") {
                    const size_t end = src_.find("]]>", pos_ + 9);
                    if (end == std::string_view::npos) {
                        return std::unexpected(std::string{"Unterminated XML CDATA section."});
                    }
                    el.text.append(src_.substr(pos_ + 9, end - (pos_ + 9)));
                    pos_ = end + 3;
                    continue;
                }
                auto child = parse_element(depth + 1);
                if (!child) return std::unexpected(child.error());
                el.children.push_back(std::move(*child));
            } else {
                const size_t tstart = pos_;
                while (pos_ < src_.size() && src_[pos_] != '<') ++pos_;
                el.text += xml_unescape(src_.substr(tstart, pos_ - tstart));
            }
        }
    }

    std::string_view src_;
    size_t           pos_        = 0;
    size_t           node_count_ = 0;
};

ExecutionPlanIcon mssql_icon_for(std::string_view phys_op) noexcept {
    if (icontains_ascii(phys_op, "Index Seek") || icontains_ascii(phys_op, "Index Scan") ||
        icontains_ascii(phys_op, "Key Lookup") || icontains_ascii(phys_op, "RID Lookup")) {
        return ExecutionPlanIcon::IndexScan;
    }
    if (icontains_ascii(phys_op, "Table Scan") || icontains_ascii(phys_op, "Constant Scan")) {
        return ExecutionPlanIcon::TableScan;
    }
    if (icontains_ascii(phys_op, "Bitmap")) return ExecutionPlanIcon::BitmapScan;
    if (icontains_ascii(phys_op, "Join") || icontains_ascii(phys_op, "Nested Loops") ||
        icontains_ascii(phys_op, "Hash Match") || icontains_ascii(phys_op, "Adaptive Join")) {
        return ExecutionPlanIcon::Join;
    }
    if (icontains_ascii(phys_op, "Sort")) return ExecutionPlanIcon::Sort;
    if (icontains_ascii(phys_op, "Aggregate") || icontains_ascii(phys_op, "Segment")) {
        return ExecutionPlanIcon::Aggregate;
    }
    if (icontains_ascii(phys_op, "Top")) return ExecutionPlanIcon::Limit;
    if (icontains_ascii(phys_op, "Spool")) return ExecutionPlanIcon::Materialize;
    if (icontains_ascii(phys_op, "Concatenation") || icontains_ascii(phys_op, "Union")) {
        return ExecutionPlanIcon::Append;
    }
    if (icontains_ascii(phys_op, "Parallelism")) return ExecutionPlanIcon::Gather;
    if (icontains_ascii(phys_op, "Insert") || icontains_ascii(phys_op, "Update") ||
        icontains_ascii(phys_op, "Delete") || icontains_ascii(phys_op, "Merge")) {
        return ExecutionPlanIcon::Modify;
    }
    if (icontains_ascii(phys_op, "Compute Scalar") || icontains_ascii(phys_op, "Filter")) {
        return ExecutionPlanIcon::Subquery;
    }
    return ExecutionPlanIcon::Other;
}

void collect_child_relops(const XmlElement& el, std::vector<const XmlElement*>& out) {
    for (const auto& ch : el.children) {
        if (ch.name == "RelOp") {
            out.push_back(&ch);
        } else {
            // RelOps are nested inside operator-specific elements like <Hash>, <NestedLoops>, <Sort>, <IndexScan>...
            collect_child_relops(ch, out);
        }
    }
}

void collect_mssql_object_name(const XmlElement& relop, std::string& out_obj) {
    for (const auto& ch : relop.children) {
        if (ch.name == "RelOp") continue;
        if (ch.name == "Object") {
            std::string parts;
            if (const auto sch = ch.attr("Schema")) parts += std::string(*sch);
            if (const auto tbl = ch.attr("Table")) {
                if (!parts.empty()) parts += '.';
                parts += std::string(*tbl);
            }
            if (const auto idx = ch.attr("Index")) {
                if (!parts.empty()) parts += '.';
                parts += std::string(*idx);
            }
            if (!parts.empty()) {
                out_obj = shorten_object(std::move(parts));
                return;
            }
        }
        collect_mssql_object_name(ch, out_obj);
        if (!out_obj.empty()) return;
    }
}

void collect_mssql_warnings(const XmlElement& relop, std::vector<std::string>& out) {
    for (const auto& ch : relop.children) {
        if (ch.name == "RelOp") continue;
        if (ch.name == "Warnings") {
            if (ch.attr("NoJoinPredicate") == std::string_view{"true"} || ch.attr("NoJoinPredicate") == std::string_view{"1"}) {
                out.emplace_back("Operator has no join predicate (NoJoinPredicate).");
            }
            for (const auto& w : ch.children) {
                if (w.name == "SpillToTempDb") {
                    out.emplace_back("Operator spilled data to tempdb (SpillToTempDb).");
                } else if (w.name == "ColumnsWithNoStatistics") {
                    out.emplace_back("Columns with no statistics.");
                } else if (w.name == "PlanAffectingConvert") {
                    std::string expr = std::string(w.attr("Expression").value_or(""));
                    out.emplace_back("Type conversion in expression may affect plan choice" + (expr.empty() ? "." : (": " + expr)));
                } else {
                    out.emplace_back("Warning: " + w.name);
                }
            }
        }
        collect_mssql_warnings(ch, out);
    }
}

void collect_mssql_missing_indexes(const XmlElement& stmt_el, std::vector<PlanIndexSuggestion>& out) {
    for (const auto& ch : stmt_el.children) {
        if (ch.name == "MissingIndexes") {
            for (const auto& grp : ch.children) {
                if (grp.name != "MissingIndexGroup") continue;
                const std::optional<double> impact = grp.attr("Impact").and_then(parse_double);
                for (const auto& mi : grp.children) {
                    if (mi.name != "MissingIndex") continue;
                    PlanIndexSuggestion sug;
                    sug.schema         = std::string(mi.attr("Schema").value_or("[dbo]"));
                    sug.table          = std::string(mi.attr("Table").value_or(""));
                    sug.impact_percent = impact;
                    for (const auto& cg : mi.children) {
                        if (cg.name != "ColumnGroup") continue;
                        const std::string_view usage = cg.attr("Usage").value_or("");
                        for (const auto& col : cg.children) {
                            if (col.name != "Column") continue;
                            if (const auto cname = col.attr("Name")) {
                                if (usage == "INCLUDE") sug.include_columns.emplace_back(*cname);
                                else sug.key_columns.emplace_back(*cname);
                            }
                        }
                    }
                    if (!sug.table.empty() && !sug.key_columns.empty()) {
                        const std::string qtbl = quote_tsql_ident(sug.schema) + "." + quote_tsql_ident(sug.table);
                        std::string keys;
                        for (size_t i = 0; i < sug.key_columns.size(); ++i) {
                            if (i > 0) keys += ", ";
                            keys += quote_tsql_ident(sug.key_columns[i]);
                        }
                        std::string incl;
                        for (size_t i = 0; i < sug.include_columns.size(); ++i) {
                            if (i > 0) incl += ", ";
                            incl += quote_tsql_ident(sug.include_columns[i]);
                        }
                        const std::string imp_str = impact ? fmt_double(*impact, 2) : "0";
                        sug.header_line = "Missing Index (Impact " + imp_str + "%): CREATE NONCLUSTERED INDEX ON " + qtbl + " (" + keys + ")";
                        if (!incl.empty()) sug.header_line += " INCLUDE (" + incl + ")";
                        sug.commented_script =
                            "/* Missing Index Details (Estimated Impact " + imp_str + "%)\n"
                            "   Review existing indexes before executing.\n"
                            "   CREATE NONCLUSTERED INDEX [IX_missing] ON " + qtbl + " (" + keys + ")" +
                            (incl.empty() ? "" : (" INCLUDE (" + incl + ")")) + ";\n*/\n";
                        out.push_back(std::move(sug));
                    }
                }
            }
        } else if (ch.name != "RelOp") {
            collect_mssql_missing_indexes(ch, out);
        }
    }
}

double mssql_relop_self_cost(const XmlElement& relop) {
    const double sub = relop.attr("EstimatedTotalSubtreeCost").and_then(parse_double).value_or(0.0);
    std::vector<const XmlElement*> child_ops;
    collect_child_relops(relop, child_ops);
    double child_sum = 0.0;
    for (const XmlElement* c : child_ops) {
        child_sum += c->attr("EstimatedTotalSubtreeCost").and_then(parse_double).value_or(0.0);
    }
    return (std::max)(0.0, sub - child_sum);
}

double mssql_relop_total_self(const XmlElement& relop) {
    double s = mssql_relop_self_cost(relop);
    std::vector<const XmlElement*> child_ops;
    collect_child_relops(relop, child_ops);
    for (const XmlElement* c : child_ops) {
        s += mssql_relop_total_self(*c);
    }
    return s;
}

ExecutionPlanOperator build_mssql_operator(const XmlElement& relop, double total_cost) {
    const std::string phys = std::string(relop.attr("PhysicalOp").value_or("Operator"));
    const std::string logi = std::string(relop.attr("LogicalOp").value_or(""));
    const double self = mssql_relop_self_cost(relop);
    const double pct  = (total_cost > 0.0) ? (100.0 * self / total_cost) : 0.0;
    const double est_rows = relop.attr("EstimateRows").and_then(parse_double).value_or(0.0);

    // Aggregate actual runtime info across threads if STATISTICS XML was active
    std::optional<double> actual_rows;
    std::optional<double> actual_execs;
    std::optional<double> actual_ms;
    if (const XmlElement* rti = relop.first_child("RunTimeInformation")) {
        double sum_rows = 0.0;
        double sum_exec = 0.0;
        double max_ms   = 0.0;
        bool has_any    = false;
        for (const auto& c : rti->children) {
            if (c.name != "RunTimeCountersPerThread") continue;
            has_any = true;
            sum_rows += c.attr("ActualRows").and_then(parse_double).value_or(0.0);
            sum_exec += c.attr("ActualExecutions").and_then(parse_double).value_or(0.0);
            max_ms    = (std::max)(max_ms, c.attr("ActualElapsedms").and_then(parse_double).value_or(0.0));
        }
        if (has_any) {
            actual_rows  = sum_rows;
            actual_execs = sum_exec;
            actual_ms    = max_ms;
        }
    }

    ExecutionPlanOperator op;
    op.title          = phys;
    op.subtitle       = (logi != phys) ? logi : std::string{};
    collect_mssql_object_name(relop, op.object_name);
    op.icon           = mssql_icon_for(phys);
    op.cost_percent   = pct;
    op.actual_rows    = actual_rows;
    op.estimated_rows = est_rows;
    if (actual_ms.has_value()) {
        op.seconds = (*actual_ms) / 1000.0;
    }

    op.properties.emplace_back("Node Type", phys);
    for (const auto& [k, v] : relop.attrs) {
        op.properties.emplace_back(k, shorten(v));
    }
    if (!op.object_name.empty()) {
        op.properties.emplace_back("Object", op.object_name);
    }
    if (actual_rows) op.properties.emplace_back("Actual Rows", fmt_double(*actual_rows));
    if (actual_execs) op.properties.emplace_back("Actual Executions", fmt_double(*actual_execs));
    if (actual_ms) op.properties.emplace_back("Actual Elapsed ms", fmt_double(*actual_ms));

    auto add_sum = [&](std::string label, std::string val) {
        if (!val.empty()) op.summary.emplace_back(std::move(label), std::move(val));
    };
    add_sum("Physical Operation", phys);
    add_sum("Logical Operation", logi);
    if (actual_rows) add_sum("Actual Number of Rows", fmt_double(*actual_rows));
    if (actual_execs) add_sum("Number of Executions", fmt_double(*actual_execs));
    add_sum("Estimated Number of Rows", fmt_double(est_rows));
    add_sum("Estimated Operator Cost", fmt_double(self, 6) + " (" + fmt_double(pct, 0) + "%)");
    if (const auto stc = relop.attr("EstimatedTotalSubtreeCost")) add_sum("Estimated Subtree Cost", std::string(*stc));
    if (const auto cpu = relop.attr("EstimateCPU")) add_sum("Estimated CPU Cost", std::string(*cpu));
    if (const auto io = relop.attr("EstimateIO")) add_sum("Estimated I/O Cost", std::string(*io));
    if (const auto rsz = relop.attr("AvgRowSize")) add_sum("Estimated Row Size", std::string(*rsz) + " B");

    collect_mssql_warnings(relop, op.warnings);
    if (!op.warnings.empty()) {
        std::string joined;
        for (size_t i = 0; i < op.warnings.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += op.warnings[i];
        }
        op.properties.emplace_back("Warnings", std::move(joined));
    }

    std::vector<const XmlElement*> child_ops;
    collect_child_relops(relop, child_ops);
    op.children.reserve(child_ops.size());
    for (const XmlElement* c : child_ops) {
        op.children.push_back(build_mssql_operator(*c, total_cost));
    }
    return op;
}

void collect_mssql_statements(const XmlElement& el, std::vector<const XmlElement*>& out) {
    if (el.name == "StmtSimple" || el.name == "StmtCond" || el.name == "StmtCursor") {
        out.push_back(&el);
        return;
    }
    for (const auto& ch : el.children) {
        collect_mssql_statements(ch, out);
    }
}

std::expected<std::vector<ExecutionPlanStatement>, std::string> parse_mssql_xml_plan(
    const RawExecutionPlan& plan,
    std::string_view fallback_sql) {
    MiniXmlParser parser(plan.raw_text);
    auto root_res = parser.parse_root();
    if (!root_res) {
        return std::unexpected(root_res.error());
    }

    std::vector<const XmlElement*> stmts;
    collect_mssql_statements(*root_res, stmts);
    if (stmts.empty()) {
        return std::unexpected(std::string{"SQL Server ShowPlan XML contains no statements."});
    }

    struct RawStmt {
        const XmlElement* el = nullptr;
        std::string       sql;
        double            cost = 0.0;
    };
    std::vector<RawStmt> parsed;
    double batch_cost = 0.0;
    for (const XmlElement* s : stmts) {
        const XmlElement* qp = s->first_child("QueryPlan");
        const XmlElement* rel = qp ? qp->first_child("RelOp") : nullptr;
        if (rel == nullptr) continue;
        RawStmt rs;
        rs.el   = s;
        rs.sql  = std::string(s->attr("StatementText").value_or(fallback_sql));
        rs.cost = s->attr("StatementSubTreeCost").and_then(parse_double)
                      .value_or(rel->attr("EstimatedTotalSubtreeCost").and_then(parse_double).value_or(0.0));
        batch_cost += rs.cost;
        parsed.push_back(std::move(rs));
    }
    if (parsed.empty()) {
        return std::unexpected(std::string{"SQL Server ShowPlan XML contains no graphical QueryPlan operator tree."});
    }

    std::vector<ExecutionPlanStatement> out;
    out.reserve(parsed.size());
    for (size_t i = 0; i < parsed.size(); ++i) {
        const XmlElement* s   = parsed[i].el;
        const XmlElement* qp  = s->first_child("QueryPlan");
        const XmlElement* rel = qp->first_child("RelOp");
        const double total_self = mssql_relop_total_self(*rel);

        ExecutionPlanStatement st;
        st.number        = static_cast<int32_t>(i + 1);
        st.query_text    = flatten_sql(parsed[i].sql);
        st.batch_percent = (batch_cost > 0.0) ? (100.0 * parsed[i].cost / batch_cost) : (100.0 / static_cast<double>(parsed.size()));
        st.source        = plan;
        collect_mssql_missing_indexes(*s, st.missing_indexes);

        ExecutionPlanOperator stmt_op;
        stmt_op.title          = std::string(s->attr("StatementType").value_or(statement_keyword(parsed[i].sql)));
        stmt_op.icon           = ExecutionPlanIcon::Statement;
        stmt_op.is_statement   = true;
        stmt_op.cost_percent   = 0.0;
        stmt_op.estimated_rows = s->attr("StatementEstRows").and_then(parse_double)
                                     .value_or(rel->attr("EstimateRows").and_then(parse_double).value_or(0.0));
        stmt_op.description    = "The SQL Server statement sent to the server.";
        stmt_op.properties.emplace_back("Statement", shorten(parsed[i].sql));
        if (const auto sc = s->attr("StatementSubTreeCost")) {
            stmt_op.properties.emplace_back("StatementSubTreeCost", std::string(*sc));
            stmt_op.summary.emplace_back("Estimated Subtree Cost", std::string(*sc));
        }
        stmt_op.summary.emplace_back("Estimated Number of Rows", fmt_double(stmt_op.estimated_rows));
        if (!st.query_text.empty()) {
            stmt_op.details.emplace_back("Statement", st.query_text);
        }
        stmt_op.children.push_back(build_mssql_operator(*rel, total_self));

        st.root = std::move(stmt_op);
        out.push_back(std::move(st));
    }
    return out;
}

} // namespace

std::vector<std::string> ExecutionPlanOperator::display_lines() const {
    std::vector<std::string> lines;
    lines.push_back(title);
    if (!subtitle.empty()) lines.push_back("(" + subtitle + ")");
    if (!object_name.empty()) lines.push_back(object_name);
    lines.push_back("Cost: " + fmt_double(cost_percent, 0) + " %");
    if (is_statement) return lines;
    if (seconds.has_value()) {
        lines.push_back(fmt_fixed(*seconds, 3) + "s");
    }
    if (actual_rows.has_value()) {
        const double pct = (estimated_rows > 0.0) ? (100.0 * (*actual_rows) / estimated_rows) : 0.0;
        lines.push_back(fmt_double(*actual_rows, 0) + " of");
        lines.push_back(fmt_double(estimated_rows, 0) + " (" + fmt_double(pct, 0) + "%)");
    }
    return lines;
}

std::optional<std::string_view> ExecutionPlanOperator::property_value(std::string_view key) const noexcept {
    for (const auto& [k, v] : properties) {
        if (k == key) return v;
    }
    return std::nullopt;
}

std::string ExecutionPlanStatement::header() const {
    return "Query " + std::to_string(number) +
           ": Query cost (relative to the batch): " + fmt_double(batch_percent, 0) + "%";
}

std::expected<std::vector<ExecutionPlanStatement>, std::string> parse_execution_plans(
    const std::vector<RawExecutionPlan>& plans,
    std::string_view fallback_query_text,
    const ExecutionPlanWarningOptions& raw_warnings) {
    if (plans.empty()) {
        return std::vector<ExecutionPlanStatement>{};
    }
    const ExecutionPlanWarningOptions warn_opts = raw_warnings.normalized();

    struct PgItem {
        RawExecutionPlan plan;
        PgRawNode        root;
        std::string      text;
        double           cost = 0.0;
    };
    std::vector<PgItem> pg_items;
    std::vector<ExecutionPlanStatement> all_statements;

    for (const RawExecutionPlan& plan : plans) {
        if (plan.raw_text.size() > kMaxExecutionPlanBytes) {
            return std::unexpected(std::string{"Plan exceeds display size limit; no partial plan is displayed."});
        }
        if (plan.source_kind == PlanSourceKind::SqlServerXml) {
            auto ms = parse_mssql_xml_plan(plan, plan.query_text.empty() ? fallback_query_text : std::string_view{plan.query_text});
            if (!ms) return std::unexpected(ms.error());
            for (auto& st : *ms) {
                all_statements.push_back(std::move(st));
            }
            continue;
        }

        // PostgreSQL EXPLAIN (FORMAT JSON)
        const auto doc = nlohmann::json::parse(plan.raw_text, nullptr, false);
        if (doc.is_discarded() || !doc.is_array() || doc.empty() || !doc[0].is_object() || !doc[0].contains("Plan")) {
            return std::unexpected(std::string{"Expected a PostgreSQL EXPLAIN (FORMAT JSON) plan array."});
        }
        size_t node_count = 0;
        auto root_node = read_pg_json_node(doc[0]["Plan"], 0, node_count);
        if (!root_node) {
            return std::unexpected(root_node.error());
        }
        std::string qtext;
        if (doc[0].contains("Query Text") && doc[0]["Query Text"].is_string()) {
            qtext = doc[0]["Query Text"].get<std::string>();
        } else if (!plan.query_text.empty()) {
            qtext = plan.query_text;
        } else {
            qtext = std::string(fallback_query_text);
        }
        const double cost = root_node->num("Total Cost").value_or(0.0);
        pg_items.push_back(PgItem{plan, std::move(*root_node), std::move(qtext), cost});
    }

    if (!pg_items.empty()) {
        double batch_cost = 0.0;
        for (const auto& it : pg_items) batch_cost += it.cost;

        for (size_t i = 0; i < pg_items.size(); ++i) {
            const auto& item = pg_items[i];
            const double total_self = pg_total_self_cost(item.root);

            ExecutionPlanStatement st;
            st.number        = static_cast<int32_t>(all_statements.size() + 1);
            st.query_text    = flatten_sql(item.text);
            st.batch_percent = (batch_cost > 0.0) ? (100.0 * item.cost / batch_cost) : (100.0 / static_cast<double>(pg_items.size()));
            st.source        = item.plan;

            ExecutionPlanOperator stmt_op;
            stmt_op.title          = statement_keyword(item.text);
            stmt_op.icon           = ExecutionPlanIcon::Statement;
            stmt_op.is_statement   = true;
            stmt_op.cost_percent   = 0.0;
            stmt_op.estimated_rows = item.root.num("Plan Rows").value_or(0.0);
            stmt_op.description    = "The statement sent to the server.";
            stmt_op.properties     = item.root.props;
            if (!item.text.empty()) {
                stmt_op.properties.emplace_back("Statement", shorten(item.text));
                stmt_op.details.emplace_back("Statement", st.query_text);
            }
            if (const auto tc = item.root.prop("Total Cost")) {
                stmt_op.summary.emplace_back("Estimated Subtree Cost", std::string(*tc));
            }
            if (const auto pr = item.root.prop("Plan Rows")) {
                stmt_op.summary.emplace_back("Estimated Number of Rows", std::string(*pr));
            }
            stmt_op.children.push_back(build_pg_operator(item.root, total_self, warn_opts, st.missing_indexes));
            st.root = std::move(stmt_op);
            all_statements.push_back(std::move(st));
        }
    }

    for (size_t i = 0; i < all_statements.size(); ++i) {
        all_statements[i].number = static_cast<int32_t>(i + 1);
    }
    return all_statements;
}

std::expected<std::vector<ExecutionPlanStatement>, std::string> parse_execution_plan_auto(
    std::string_view content,
    const ExecutionPlanWarningOptions& warnings) {
    std::string_view sv = content;
    if (sv.size() >= 3 &&
        static_cast<unsigned char>(sv[0]) == 0xEF &&
        static_cast<unsigned char>(sv[1]) == 0xBB &&
        static_cast<unsigned char>(sv[2]) == 0xBF) {
        sv.remove_prefix(3);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    if (sv.empty()) {
        return std::unexpected(std::string{"Execution plan file is empty."});
    }

    RawExecutionPlan raw;
    raw.raw_text = std::string(sv);
    if (sv.front() == '<') {
        raw.source_kind = PlanSourceKind::SqlServerXml;
        raw.analyzed    = icontains_ascii(sv, "RunTimeInformation");
    } else if (sv.front() == '[') {
        raw.source_kind = PlanSourceKind::PostgreSqlJson;
        raw.analyzed    = icontains_ascii(sv, "\"Actual Rows\"");
    } else if (sv.front() == '{') {
        raw.source_kind = PlanSourceKind::PostgreSqlJson;
        raw.raw_text    = "[" + std::string(sv) + "]";
        raw.analyzed    = icontains_ascii(sv, "\"Actual Rows\"");
    } else {
        return std::unexpected(std::string{"Unrecognized execution plan format (expected PostgreSQL JSON or SQL Server ShowPlan XML)."});
    }
    return parse_execution_plans({std::move(raw)}, {}, warnings);
}

std::expected<void, std::string> save_raw_execution_plan(
    const RawExecutionPlan& plan,
    const std::filesystem::path& destination) {
    if (destination.empty()) {
        return std::unexpected(std::string{"Choose a destination path."});
    }
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
    }
    std::string uid = generate_uuid();
    std::erase(uid, '-');
    const std::filesystem::path partial =
        destination.parent_path() / (destination.filename().string() + "." + uid + ".partial");
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return std::unexpected(std::string{"Could not open execution plan file for writing."});
        }
        out.write(plan.raw_text.data(), static_cast<std::streamsize>(plan.raw_text.size()));
        out.flush();
        if (!out.good()) {
            std::filesystem::remove(partial, ec);
            return std::unexpected(std::string{"Failed writing execution plan file."});
        }
    }
    std::filesystem::rename(partial, destination, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(destination, rm_ec);
        ec.clear();
        std::filesystem::rename(partial, destination, ec);
        if (ec) {
            std::filesystem::remove(partial, rm_ec);
            return std::unexpected(std::string{"Could not finalize saved execution plan file."});
        }
    }
    return {};
}

std::expected<std::vector<ExecutionPlanStatement>, std::string> load_execution_plan_file(
    const std::filesystem::path& source_path,
    const ExecutionPlanWarningOptions& warnings) {
    const auto content = read_text_file(source_path);
    if (!content.has_value()) {
        return std::unexpected(std::string{"Could not read execution plan file."});
    }
    return parse_execution_plan_auto(*content, warnings);
}

// ===========================================================================
// ExecutionPlanSearch free functions
// ===========================================================================

namespace {
void visit_ops(const ExecutionPlanOperator& op, std::vector<const ExecutionPlanOperator*>& out) {
    out.push_back(&op);
    for (const auto& ch : op.children) {
        visit_ops(ch, out);
    }
}
} // namespace

std::vector<const ExecutionPlanOperator*> collect_plan_operators(
    const std::vector<ExecutionPlanStatement>& statements) {
    std::vector<const ExecutionPlanOperator*> out;
    for (const auto& st : statements) {
        visit_ops(st.root, out);
    }
    return out;
}

std::vector<std::string> collect_plan_property_names(
    const std::vector<const ExecutionPlanOperator*>& operators) {
    std::set<std::string> unique;
    for (const ExecutionPlanOperator* op : operators) {
        if (op == nullptr) continue;
        for (const auto& [k, _] : op->properties) {
            unique.insert(k);
        }
    }
    std::vector<std::string> out(unique.begin(), unique.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return icompare_ascii(a, b) < 0;
    });
    return out;
}

bool plan_operator_matches(
    const ExecutionPlanOperator& op,
    std::string_view property,
    ExecutionPlanComparison cmp,
    std::string_view value) {
    if (op.is_statement && property != "Statement") {
        return false;
    }
    const auto actual_opt = op.property_value(property);
    if (!actual_opt.has_value()) {
        return false;
    }
    const std::string_view actual = *actual_opt;
    if (cmp == ExecutionPlanComparison::Contains) {
        return icontains_ascii(actual, value);
    }
    const auto na = parse_double(actual);
    const auto nb = parse_double(value);
    int order = 0;
    if (na.has_value() && nb.has_value()) {
        order = (*na < *nb) ? -1 : (*na > *nb ? 1 : 0);
    } else {
        order = icompare_ascii(actual, value);
    }
    switch (cmp) {
    case ExecutionPlanComparison::Equal:       return order == 0;
    case ExecutionPlanComparison::GreaterThan: return order > 0;
    case ExecutionPlanComparison::LessThan:    return order < 0;
    case ExecutionPlanComparison::Contains:
    default:                                   return false;
    }
}

const ExecutionPlanOperator* find_next_plan_operator(
    const std::vector<const ExecutionPlanOperator*>& operators,
    const ExecutionPlanOperator* current,
    std::string_view property,
    ExecutionPlanComparison cmp,
    std::string_view value,
    bool forward) {
    const size_t n = operators.size();
    if (n == 0) return nullptr;

    ptrdiff_t start = -1;
    if (current != nullptr) {
        for (size_t i = 0; i < n; ++i) {
            if (operators[i] == current) {
                start = static_cast<ptrdiff_t>(i);
                break;
            }
        }
    }
    if (start < 0) {
        start = forward ? -1 : static_cast<ptrdiff_t>(n);
    }

    for (size_t step = 1; step <= n; ++step) {
        const ptrdiff_t raw = start + (forward ? static_cast<ptrdiff_t>(step) : -static_cast<ptrdiff_t>(step));
        const size_t idx = static_cast<size_t>(((raw % static_cast<ptrdiff_t>(n)) + static_cast<ptrdiff_t>(n)) % static_cast<ptrdiff_t>(n));
        if (operators[idx] != nullptr && plan_operator_matches(*operators[idx], property, cmp, value)) {
            return operators[idx];
        }
    }
    return nullptr;
}

} // namespace capybaradb::core
