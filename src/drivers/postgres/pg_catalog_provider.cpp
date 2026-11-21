#include "drivers/postgres/pg_catalog_provider.hpp"

#include "drivers/postgres/pg_driver.hpp"

#include <libpq-fe.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace capybaradb::drivers {
namespace {

std::string quote_pg_ident(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 4);
    out.push_back('"');
    for (const char ch : ident) {
        if (ch == '"') {
            out.push_back('"');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string cell_str(const PGresult* res, int row, int col) {
    if (res == nullptr || PQgetisnull(res, row, col) != 0) {
        return {};
    }
    const char* p = PQgetvalue(res, row, col);
    return p != nullptr ? std::string{p} : std::string{};
}

bool cell_bool(const PGresult* res, int row, int col) {
    const std::string s = cell_str(res, row, col);
    return !s.empty() && (s[0] == 't' || s[0] == '1');
}

uint64_t cell_u64(const PGresult* res, int row, int col) {
    const std::string s = cell_str(res, row, col);
    return s.empty() ? 0ull : std::strtoull(s.c_str(), nullptr, 10);
}

std::expected<std::vector<CatalogNodeData>, DbError> list_diagrams(
    const ConnectionConfig& cfg,
    std::string_view database,
    core::CancelToken& cancel) {
    auto conn = PgDriver::open_connection(cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    auto installed = PgDriver::exec_params_cancelable(
        conn->get(),
        "SELECT pg_catalog.to_regclass('capybaradb.diagrams') IS NOT NULL",
        {},
        cancel,
        15);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    if (PQntuples(installed->get()) == 0 || !cell_bool(installed->get(), 0, 0)) {
        return std::vector<CatalogNodeData>{};
    }

    auto res = PgDriver::exec_params_cancelable(
        conn->get(),
        "SELECT diagram_id, name, owner FROM capybaradb.diagrams ORDER BY name, diagram_id",
        {},
        cancel,
        15);
    if (!res) {
        return std::unexpected(res.error());
    }

    std::vector<CatalogNodeData> out;
    const int rows = PQntuples(res->get());
    out.reserve(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        CatalogNodeData node;
        node.kind     = CatalogKind::DatabaseDiagram;
        node.oid      = cell_u64(res->get(), r, 0);
        node.name     = cell_str(res->get(), r, 1);
        node.owner    = cell_str(res->get(), r, 2);
        node.database = std::string(database);
        out.push_back(std::move(node));
    }
    return out;
}

} // namespace

std::expected<std::vector<CatalogNodeData>, DbError> PgCatalogProvider::list_children(
    const ConnectionConfig& base_cfg,
    const CatalogNodeData& parent,
    core::CancelToken& cancel) {
    auto folder = [&](CatalogKind kind, std::string name) {
        return CatalogNodeData::make_folder(kind, std::move(name), parent.database, parent.schema, parent.oid);
    };

    switch (parent.kind) {
    case CatalogKind::Server:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Databases, "Databases"),
            folder(CatalogKind::Roles, "Login/Group Roles"),
        };
    case CatalogKind::Database:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::DatabaseDiagrams, "Database Diagrams"),
            folder(CatalogKind::Schemas, "Schemas"),
        };
    case CatalogKind::Schema:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Tables, "Tables"),
            folder(CatalogKind::Views, "Views"),
            folder(CatalogKind::MaterializedViews, "Materialized Views"),
            folder(CatalogKind::Sequences, "Sequences"),
            folder(CatalogKind::Functions, "Functions"),
            folder(CatalogKind::Procedures, "Procedures"),
            folder(CatalogKind::Enums, "Enums"),
            folder(CatalogKind::Domains, "Domains"),
        };
    case CatalogKind::Relation:
        if (parent.relation_type == 'r' || parent.relation_type == 'p') {
            return std::vector<CatalogNodeData>{
                folder(CatalogKind::Columns, "Columns"),
                folder(CatalogKind::Keys, "Keys"),
                folder(CatalogKind::Constraints, "Constraints"),
                folder(CatalogKind::Triggers, "Triggers"),
                folder(CatalogKind::Indexes, "Indexes"),
            };
        }
        if (parent.relation_type == 'v') {
            return std::vector<CatalogNodeData>{
                folder(CatalogKind::Columns, "Columns"),
                folder(CatalogKind::Triggers, "Triggers"),
                folder(CatalogKind::Indexes, "Indexes"),
            };
        }
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Columns, "Columns"),
            folder(CatalogKind::Indexes, "Indexes"),
        };
    case CatalogKind::DatabaseDiagrams: {
        const ConnectionConfig target_cfg = parent.database.empty()
            ? base_cfg
            : base_cfg.with_database(parent.database);
        return list_diagrams(target_cfg, parent.database.empty() ? base_cfg.database : parent.database, cancel);
    }
    default:
        break;
    }

    if (!parent.can_expand()) {
        return std::vector<CatalogNodeData>{};
    }

    const std::string target_db = parent.database.empty() ? base_cfg.database : parent.database;
    const ConnectionConfig conn_cfg = base_cfg.with_database(target_db);
    auto conn = PgDriver::open_connection(conn_cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    const std::string oid_str = std::to_string(parent.oid);
    const std::string schema_str = parent.schema.empty() ? std::string{"public"} : parent.schema;
    std::string kind_str;
    std::string kinds_array;
    std::vector<const char*> params;
    const char* sql = nullptr;
    CatalogKind child_kind = CatalogKind::Relation;

    switch (parent.kind) {
    case CatalogKind::Databases:
        sql = "SELECT oid, datname || CASE WHEN datallowconn THEN '' ELSE ' (Offline)' END, "
              "datname, NOT datallowconn FROM pg_catalog.pg_database WHERE NOT datistemplate ORDER BY datname";
        child_kind = CatalogKind::Database;
        break;
    case CatalogKind::Keys:
        sql = "SELECT oid, conname FROM pg_catalog.pg_constraint "
              "WHERE conrelid=$1::oid AND contype IN ('p','u','f') "
              "ORDER BY CASE contype WHEN 'p' THEN 0 WHEN 'u' THEN 1 ELSE 2 END, conname";
        params.push_back(oid_str.c_str());
        child_kind = CatalogKind::Key;
        break;
    case CatalogKind::Constraints:
        sql = "SELECT oid, conname FROM pg_catalog.pg_constraint "
              "WHERE conrelid=$1::oid AND contype IN ('c','x') ORDER BY conname";
        params.push_back(oid_str.c_str());
        child_kind = CatalogKind::Constraint;
        break;
    case CatalogKind::Triggers:
        sql = "SELECT oid, tgname FROM pg_catalog.pg_trigger "
              "WHERE tgrelid=$1::oid AND NOT tgisinternal ORDER BY tgname";
        params.push_back(oid_str.c_str());
        child_kind = CatalogKind::Trigger;
        break;
    case CatalogKind::Schemas:
        sql = "SELECT oid, nspname FROM pg_catalog.pg_namespace "
              "WHERE nspname <> 'information_schema' AND nspname !~ '^pg_' ORDER BY nspname";
        child_kind = CatalogKind::Schema;
        break;
    case CatalogKind::Roles:
        sql = "SELECT oid, rolname || CASE WHEN rolcanlogin THEN ' (login)' ELSE '' END, rolname "
              "FROM pg_catalog.pg_roles ORDER BY rolname";
        child_kind = CatalogKind::Role;
        break;
    case CatalogKind::Enums:
    case CatalogKind::Domains:
        sql = "SELECT t.oid, t.typname, t.typtype::text FROM pg_catalog.pg_type t "
              "JOIN pg_catalog.pg_namespace n ON n.oid=t.typnamespace "
              "WHERE n.nspname=$1 AND t.typtype::text=$2 ORDER BY t.typname, t.oid";
        kind_str = (parent.kind == CatalogKind::Enums) ? "e" : "d";
        params.push_back(schema_str.c_str());
        params.push_back(kind_str.c_str());
        child_kind = CatalogKind::DataType;
        break;
    case CatalogKind::Functions:
    case CatalogKind::Procedures:
        sql = "SELECT p.oid, p.proname || '(' || pg_catalog.pg_get_function_identity_arguments(p.oid) || ')', "
              "pg_catalog.pg_get_userbyid(p.proowner) "
              "FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
              "WHERE n.nspname=$1 AND p.prokind::text=$2 ORDER BY p.proname, p.oid";
        kind_str = (parent.kind == CatalogKind::Procedures) ? "p" : "f";
        params.push_back(schema_str.c_str());
        params.push_back(kind_str.c_str());
        child_kind = CatalogKind::Routine;
        break;
    case CatalogKind::Columns:
        sql = "SELECT attnum::oid, attname || ' (' || pg_catalog.format_type(atttypid, atttypmod) || "
              "CASE WHEN attnotnull THEN ', not null)' ELSE ', null)' END "
              "FROM pg_catalog.pg_attribute WHERE attrelid=$1::oid AND attnum>0 AND NOT attisdropped "
              "ORDER BY attnum";
        params.push_back(oid_str.c_str());
        child_kind = CatalogKind::Column;
        break;
    case CatalogKind::Indexes:
        sql = "SELECT c.oid, c.relname FROM pg_catalog.pg_index i "
              "JOIN pg_catalog.pg_class c ON c.oid=i.indexrelid WHERE i.indrelid=$1::oid ORDER BY c.relname";
        params.push_back(oid_str.c_str());
        child_kind = CatalogKind::Index;
        break;
    case CatalogKind::Tables:
    case CatalogKind::Views:
    case CatalogKind::MaterializedViews:
    case CatalogKind::Sequences:
        sql = "SELECT c.oid, c.relname, c.relkind::text, pg_catalog.pg_get_userbyid(c.relowner) "
              "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
              "WHERE n.nspname=$1 AND c.relkind::text = ANY($2::text[]) ORDER BY c.relname";
        if (parent.kind == CatalogKind::Tables) {
            kinds_array = "{r,p,f}";
        } else if (parent.kind == CatalogKind::Views) {
            kinds_array = "{v}";
        } else if (parent.kind == CatalogKind::MaterializedViews) {
            kinds_array = "{m}";
        } else {
            kinds_array = "{S}";
        }
        params.push_back(schema_str.c_str());
        params.push_back(kinds_array.c_str());
        child_kind = CatalogKind::Relation;
        break;
    default:
        return std::vector<CatalogNodeData>{};
    }

    auto res = PgDriver::exec_params_cancelable(conn->get(), sql, params, cancel, 15);
    if (!res) {
        return std::unexpected(res.error());
    }

    std::vector<CatalogNodeData> out;
    const int rows = PQntuples(res->get());
    out.reserve(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        CatalogNodeData item;
        item.kind = child_kind;
        item.oid  = cell_u64(res->get(), r, 0);
        item.name = cell_str(res->get(), r, 1);

        if (child_kind == CatalogKind::Database) {
            item.database   = cell_str(res->get(), r, 2);
            item.is_offline = cell_bool(res->get(), r, 3);
        } else {
            item.database = target_db;
        }

        if (child_kind == CatalogKind::Schema) {
            item.schema = item.name;
        } else if (child_kind != CatalogKind::Database && child_kind != CatalogKind::Role) {
            item.schema = parent.schema;
        }

        if (child_kind == CatalogKind::Relation) {
            const std::string rt = cell_str(res->get(), r, 2);
            item.relation_type = rt.empty() ? '\0' : rt[0];
            item.owner         = cell_str(res->get(), r, 3);
        } else if (child_kind == CatalogKind::DataType) {
            const std::string dtk = cell_str(res->get(), r, 2);
            item.data_type_kind = dtk.empty() ? '\0' : dtk[0];
        } else if (child_kind == CatalogKind::Routine) {
            item.routine_kind = (parent.kind == CatalogKind::Procedures) ? 'p' : 'f';
            item.owner        = cell_str(res->get(), r, 2);
        } else if (child_kind == CatalogKind::Role) {
            item.role_name = cell_str(res->get(), r, 2);
        }

        out.push_back(std::move(item));
    }
    return out;
}

std::expected<std::vector<std::string>, DbError> PgCatalogProvider::list_databases(
    const ConnectionConfig& base_cfg,
    core::CancelToken& cancel) {
    const CatalogNodeData root = CatalogNodeData::make_folder(CatalogKind::Databases, "Databases", base_cfg.database);
    auto children = list_children(base_cfg, root, cancel);
    if (!children) {
        return std::unexpected(children.error());
    }
    std::vector<std::string> dbs;
    dbs.reserve(children->size());
    for (const auto& entry : *children) {
        if (!entry.is_offline && !entry.database.empty()) {
            dbs.push_back(entry.database);
        }
    }
    return dbs;
}

std::expected<std::vector<std::string>, DbError> PgCatalogProvider::read_table_columns(
    const ConnectionConfig& base_cfg,
    std::string_view database,
    std::string_view schema,
    std::string_view table,
    core::CancelToken& cancel) {
    const ConnectionConfig cfg = database.empty() ? base_cfg : base_cfg.with_database(std::string(database));
    auto conn = PgDriver::open_connection(cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    static constexpr const char* kSql =
        "SELECT a.attname FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
        "WHERE n.nspname = $1 AND c.relname = $2 AND a.attnum > 0 AND NOT a.attisdropped "
        "ORDER BY a.attnum";
    const std::string sch(schema);
    const std::string tbl(table);
    const std::array<const char*, 2> params{sch.c_str(), tbl.c_str()};
    auto res = PgDriver::exec_params_cancelable(conn->get(), kSql, params, cancel, 15);
    if (!res) {
        return std::unexpected(res.error());
    }

    std::vector<std::string> cols;
    const int rows = PQntuples(res->get());
    cols.reserve(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        cols.push_back(cell_str(res->get(), r, 0));
    }
    return cols;
}

std::string PgCatalogProvider::generate_select_top_sql(
    std::string_view schema,
    std::string_view object,
    std::span<const std::string> columns,
    uint32_t top_n) const {
    const uint32_t limit = top_n > 0 ? top_n : 1000u;
    std::string sql = "SELECT ";
    if (columns.empty()) {
        sql += "*";
    } else {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                sql += "\n      ,";
            }
            sql += quote_pg_ident(columns[i]);
        }
    }
    sql += "\n  FROM ";
    sql += quote_pg_ident(schema);
    sql += ".";
    sql += quote_pg_ident(object);
    sql += "\n LIMIT ";
    sql += std::to_string(limit);
    sql += ";\n";
    return sql;
}

namespace {

std::vector<std::string> parse_pg_text_array(std::string_view raw) {
    std::vector<std::string> out;
    if (raw.size() < 2 || raw.front() != '{' || raw.back() != '}') {
        return out;
    }
    raw.remove_prefix(1);
    raw.remove_suffix(1);
    size_t i = 0;
    while (i < raw.size()) {
        std::string elem;
        if (raw[i] == '"') {
            ++i;
            while (i < raw.size()) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    elem.push_back(raw[i + 1]);
                    i += 2;
                } else if (raw[i] == '"') {
                    ++i;
                    break;
                } else {
                    elem.push_back(raw[i++]);
                }
            }
            if (i < raw.size() && raw[i] == ',') ++i;
        } else {
            const size_t start = i;
            while (i < raw.size() && raw[i] != ',') ++i;
            elem.assign(raw.substr(start, i - start));
            if (i < raw.size() && raw[i] == ',') ++i;
        }
        out.push_back(std::move(elem));
    }
    return out;
}

} // namespace

std::expected<core::SqlCompletionCatalog, DbError> PgCatalogProvider::load_completion_catalog(
    const ConnectionConfig& base_cfg,
    core::CancelToken& cancel) {
    auto conn = PgDriver::open_connection(base_cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    core::SqlCompletionCatalog cat;
    cat.is_tsql  = false;
    cat.database = base_cfg.database;

    static constexpr const char* kVisible =
        "n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp\\_%'";

    // 1. search_path
    if (auto r = PgDriver::exec_params_cancelable(conn->get(), "SELECT unnest(pg_catalog.current_schemas(true))::text", {}, cancel, 20)) {
        const int n = PQntuples(r->get());
        for (int i = 0; i < n; ++i) cat.search_path.push_back(cell_str(r->get(), i, 0));
    } else {
        return std::unexpected(r.error());
    }

    // 2. schemas
    {
        const std::string sql = std::string("SELECT n.nspname::text FROM pg_catalog.pg_namespace n WHERE ") + kVisible + " ORDER BY 1";
        auto r = PgDriver::exec_params_cancelable(conn->get(), sql.c_str(), {}, cancel, 20);
        if (!r) return std::unexpected(r.error());
        const int n = PQntuples(r->get());
        for (int i = 0; i < n; ++i) cat.schemas.push_back(cell_str(r->get(), i, 0));
    }

    // 3. relations
    {
        const std::string sql =
            std::string("SELECT n.nspname::text, c.relname::text, c.relkind IN ('v','m') "
                        "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "WHERE c.relkind IN ('r','p','v','m','f') AND ") +
            kVisible + " ORDER BY n.nspname = 'pg_catalog', 1, 2 LIMIT " +
            std::to_string(core::SqlCompletionCatalog::kMaxRelations);
        auto r = PgDriver::exec_params_cancelable(conn->get(), sql.c_str(), {}, cancel, 25);
        if (!r) return std::unexpected(r.error());
        const int n = PQntuples(r->get());
        cat.relations.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            cat.relations.push_back(core::SqlCatalogRelation{
                .schema  = cell_str(r->get(), i, 0),
                .name    = cell_str(r->get(), i, 1),
                .is_view = cell_bool(r->get(), i, 2),
            });
        }
    }

    // 4. columns
    {
        const std::string sql =
            std::string("SELECT n.nspname::text, c.relname::text, a.attname::text, "
                        "pg_catalog.format_type(a.atttypid, a.atttypmod), NOT a.attnotnull "
                        "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid = a.attrelid "
                        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "WHERE a.attnum > 0 AND NOT a.attisdropped AND c.relkind IN ('r','p','v','m','f') AND ") +
            kVisible + " ORDER BY n.nspname = 'pg_catalog', 1, 2, a.attnum LIMIT " +
            std::to_string(core::SqlCompletionCatalog::kMaxColumns);
        auto r = PgDriver::exec_params_cancelable(conn->get(), sql.c_str(), {}, cancel, 25);
        if (!r) return std::unexpected(r.error());
        const int n = PQntuples(r->get());
        cat.all_columns.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            cat.append_column(
                cell_str(r->get(), i, 0),
                cell_str(r->get(), i, 1),
                core::SqlCatalogColumn{
                    .name     = cell_str(r->get(), i, 2),
                    .type     = cell_str(r->get(), i, 3),
                    .nullable = cell_bool(r->get(), i, 4),
                });
        }
    }

    // 5. functions
    {
        const std::string sql =
            std::string("SELECT n.nspname::text, p.proname::text, pg_catalog.pg_get_function_arguments(p.oid), "
                        "COALESCE(pg_catalog.pg_get_function_result(p.oid), ''), p.prokind = 'f' AND p.proretset "
                        "FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
                        "WHERE p.prokind IN ('f','a','w') AND ") +
            kVisible + " ORDER BY n.nspname = 'pg_catalog', 1, 2 LIMIT " +
            std::to_string(core::SqlCompletionCatalog::kMaxFunctions);
        auto r = PgDriver::exec_params_cancelable(conn->get(), sql.c_str(), {}, cancel, 25);
        if (!r) return std::unexpected(r.error());
        const int n = PQntuples(r->get());
        cat.functions.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            cat.functions.push_back(core::SqlCatalogFunction{
                .schema       = cell_str(r->get(), i, 0),
                .name         = cell_str(r->get(), i, 1),
                .arguments    = cell_str(r->get(), i, 2),
                .result       = cell_str(r->get(), i, 3),
                .returns_rows = cell_bool(r->get(), i, 4),
            });
        }
    }

    // 6. foreign keys
    {
        const std::string sql =
            std::string("SELECT con.conname::text, n.nspname::text, c.relname::text, "
                        "(ARRAY(SELECT a.attname::text FROM unnest(con.conkey) WITH ORDINALITY k(attnum, pos) "
                        "JOIN pg_catalog.pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = k.attnum ORDER BY k.pos))::text, "
                        "tn.nspname::text, t.relname::text, "
                        "(ARRAY(SELECT a.attname::text FROM unnest(con.confkey) WITH ORDINALITY k(attnum, pos) "
                        "JOIN pg_catalog.pg_attribute a ON a.attrelid = con.confrelid AND a.attnum = k.attnum ORDER BY k.pos))::text "
                        "FROM pg_catalog.pg_constraint con "
                        "JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
                        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                        "JOIN pg_catalog.pg_class t ON t.oid = con.confrelid "
                        "JOIN pg_catalog.pg_namespace tn ON tn.oid = t.relnamespace "
                        "WHERE con.contype = 'f' AND con.conparentid = 0 AND ") +
            kVisible + " ORDER BY 2, 3, 1 LIMIT " +
            std::to_string(core::SqlCompletionCatalog::kMaxForeignKeys);
        auto r = PgDriver::exec_params_cancelable(conn->get(), sql.c_str(), {}, cancel, 25);
        if (!r) return std::unexpected(r.error());
        const int n = PQntuples(r->get());
        cat.foreign_keys.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            cat.foreign_keys.push_back(core::SqlCatalogForeignKey{
                .name           = cell_str(r->get(), i, 0),
                .schema         = cell_str(r->get(), i, 1),
                .table          = cell_str(r->get(), i, 2),
                .columns        = parse_pg_text_array(cell_str(r->get(), i, 3)),
                .target_schema  = cell_str(r->get(), i, 4),
                .target_table   = cell_str(r->get(), i, 5),
                .target_columns = parse_pg_text_array(cell_str(r->get(), i, 6)),
            });
        }
    }

    return cat;
}

} // namespace capybaradb::drivers
