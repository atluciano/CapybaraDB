#include "drivers/sqlite/sqlite_catalog_provider.hpp"

#include "core/completion/sql_completion.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace capybaradb::drivers {
namespace {

struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
    }
};
using UniqueSqlite = std::unique_ptr<sqlite3, SqliteCloser>;

std::expected<UniqueSqlite, DbError> open_readonly(const ConnectionConfig& cfg, core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }
    const std::string path = !cfg.database.empty()
        ? cfg.database
        : (!cfg.host.empty() && cfg.host != "localhost" ? cfg.host : ":memory:");

    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(
        path.c_str(),
        &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI,
        nullptr);

    if (rc != SQLITE_OK) {
        std::string msg = db != nullptr ? sqlite3_errmsg(db) : "Failed to open SQLite database.";
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        return std::unexpected(DbError{.message = std::move(msg), .native_code = rc});
    }

    return UniqueSqlite(db);
}

std::string quote_sqlite_ident(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 2);
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

} // namespace

std::expected<std::vector<std::string>, DbError> SqliteCatalogProvider::list_databases(
    const ConnectionConfig& base_cfg,
    core::CancelToken& cancel) {
    auto db = open_readonly(base_cfg, cancel);
    if (!db) {
        return std::unexpected(db.error());
    }

    std::vector<std::string> dbs;
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db->get(), "PRAGMA database_list;", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(DbError{.message = sqlite3_errmsg(db->get()), .native_code = rc});
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancel.is_canceled()) {
            sqlite3_finalize(stmt);
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name != nullptr) {
            dbs.emplace_back(name);
        }
    }
    sqlite3_finalize(stmt);

    if (dbs.empty()) {
        dbs.emplace_back("main");
    }
    return dbs;
}

std::expected<std::vector<CatalogNodeData>, DbError> SqliteCatalogProvider::list_children(
    const ConnectionConfig& base_cfg,
    const CatalogNodeData& parent,
    core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }

    auto folder = [&](CatalogKind kind, std::string name) {
        return CatalogNodeData::make_folder(kind, std::move(name), parent.database, parent.schema, parent.oid);
    };

    switch (parent.kind) {
    case CatalogKind::Server:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Databases, "Databases"),
        };

    case CatalogKind::Databases: {
        auto dbs = list_databases(base_cfg, cancel);
        if (!dbs) {
            return std::unexpected(dbs.error());
        }
        std::vector<CatalogNodeData> nodes;
        nodes.reserve(dbs->size());
        for (const auto& db_name : *dbs) {
            CatalogNodeData node;
            node.kind     = CatalogKind::Database;
            node.name     = db_name;
            node.database = db_name;
            nodes.push_back(std::move(node));
        }
        return nodes;
    }

    case CatalogKind::Database:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Tables, "Tables"),
            folder(CatalogKind::Views, "Views"),
        };

    case CatalogKind::Tables: {
        auto db = open_readonly(base_cfg, cancel);
        if (!db) {
            return std::unexpected(db.error());
        }
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name;";
        if (sqlite3_prepare_v2(db->get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
        }
        std::vector<CatalogNodeData> nodes;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (cancel.is_canceled()) {
                sqlite3_finalize(stmt);
                return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
            }
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            CatalogNodeData node;
            node.kind          = CatalogKind::Relation;
            node.name          = name ? name : "";
            node.relation_type = 'r';
            node.database      = parent.database;
            nodes.push_back(std::move(node));
        }
        sqlite3_finalize(stmt);
        return nodes;
    }

    case CatalogKind::Views: {
        auto db = open_readonly(base_cfg, cancel);
        if (!db) {
            return std::unexpected(db.error());
        }
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT name FROM sqlite_schema WHERE type = 'view' ORDER BY name;";
        if (sqlite3_prepare_v2(db->get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
        }
        std::vector<CatalogNodeData> nodes;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (cancel.is_canceled()) {
                sqlite3_finalize(stmt);
                return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
            }
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            CatalogNodeData node;
            node.kind          = CatalogKind::Relation;
            node.name          = name ? name : "";
            node.relation_type = 'v';
            node.database      = parent.database;
            nodes.push_back(std::move(node));
        }
        sqlite3_finalize(stmt);
        return nodes;
    }

    case CatalogKind::Relation:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Columns, "Columns"),
            folder(CatalogKind::Indexes, "Indexes"),
        };

    case CatalogKind::Columns: {
        auto db = open_readonly(base_cfg, cancel);
        if (!db) {
            return std::unexpected(db.error());
        }
        const std::string pragma_sql = "PRAGMA table_info(" + quote_sqlite_ident(parent.name) + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db->get(), pragma_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
        }
        std::vector<CatalogNodeData> nodes;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (cancel.is_canceled()) {
                sqlite3_finalize(stmt);
                return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
            }
            const auto* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const auto* col_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const int notnull    = sqlite3_column_int(stmt, 3);
            const int pk         = sqlite3_column_int(stmt, 5);

            std::string desc = col_name ? col_name : "";
            desc += " (";
            desc += (col_type && *col_type) ? col_type : "ANY";
            desc += (notnull ? ", not null" : ", null");
            if (pk > 0) {
                desc += ", PK";
            }
            desc += ")";

            CatalogNodeData node;
            node.kind     = CatalogKind::Column;
            node.name     = std::move(desc);
            node.database = parent.database;
            nodes.push_back(std::move(node));
        }
        sqlite3_finalize(stmt);
        return nodes;
    }

    case CatalogKind::Indexes: {
        auto db = open_readonly(base_cfg, cancel);
        if (!db) {
            return std::unexpected(db.error());
        }
        const std::string pragma_sql = "PRAGMA index_list(" + quote_sqlite_ident(parent.name) + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db->get(), pragma_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
        }
        std::vector<CatalogNodeData> nodes;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (cancel.is_canceled()) {
                sqlite3_finalize(stmt);
                return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
            }
            const auto* idx_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            CatalogNodeData node;
            node.kind     = CatalogKind::Index;
            node.name     = idx_name ? idx_name : "";
            node.database = parent.database;
            nodes.push_back(std::move(node));
        }
        sqlite3_finalize(stmt);
        return nodes;
    }

    default:
        break;
    }

    return std::vector<CatalogNodeData>{};
}

std::expected<std::vector<std::string>, DbError> SqliteCatalogProvider::read_table_columns(
    const ConnectionConfig& base_cfg,
    std::string_view /*database*/,
    std::string_view /*schema*/,
    std::string_view table,
    core::CancelToken& cancel) {
    auto db = open_readonly(base_cfg, cancel);
    if (!db) {
        return std::unexpected(db.error());
    }

    const std::string pragma_sql = "PRAGMA table_info(" + quote_sqlite_ident(table) + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db->get(), pragma_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
    }

    std::vector<std::string> cols;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancel.is_canceled()) {
            sqlite3_finalize(stmt);
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        const auto* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (col_name != nullptr) {
            cols.emplace_back(col_name);
        }
    }
    sqlite3_finalize(stmt);
    return cols;
}

std::string SqliteCatalogProvider::generate_select_top_sql(
    std::string_view /*schema*/,
    std::string_view object,
    std::span<const std::string> columns,
    uint32_t top_n) const {
    std::string sql = "SELECT ";
    if (columns.empty()) {
        sql += "*";
    } else {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += quote_sqlite_ident(columns[i]);
        }
    }
    sql += "\nFROM " + quote_sqlite_ident(object);
    if (top_n > 0) {
        sql += "\nLIMIT " + std::to_string(top_n);
    }
    sql += ";";
    return sql;
}

std::expected<core::SqlCompletionCatalog, DbError> SqliteCatalogProvider::load_completion_catalog(
    const ConnectionConfig& base_cfg,
    core::CancelToken& cancel) {
    auto db = open_readonly(base_cfg, cancel);
    if (!db) {
        return std::unexpected(db.error());
    }

    core::SqlCompletionCatalog cat;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT type, name FROM sqlite_schema WHERE type IN ('table', 'view') AND name NOT LIKE 'sqlite_%';";
    if (sqlite3_prepare_v2(db->get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::unexpected(DbError{.message = sqlite3_errmsg(db->get())});
    }

    std::vector<std::pair<std::string, bool>> relations;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancel.is_canceled()) {
            sqlite3_finalize(stmt);
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name != nullptr) {
            const bool is_view = (type != nullptr && std::string_view{type} == "view");
            relations.emplace_back(name, is_view);
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& [rel_name, is_view] : relations) {
        cat.relations.push_back(core::SqlCatalogRelation{
            .schema       = "main",
            .name         = rel_name,
            .is_view      = is_view,
            .first_column = static_cast<uint32_t>(cat.all_columns.size()),
            .column_count = 0u,
        });

        const std::string pragma_sql = "PRAGMA table_info(" + quote_sqlite_ident(rel_name) + ");";
        sqlite3_stmt* cstmt = nullptr;
        if (sqlite3_prepare_v2(db->get(), pragma_sql.c_str(), -1, &cstmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(cstmt) == SQLITE_ROW) {
                const auto* cname = reinterpret_cast<const char*>(sqlite3_column_text(cstmt, 1));
                const auto* ctype = reinterpret_cast<const char*>(sqlite3_column_text(cstmt, 2));
                const int notnull = sqlite3_column_int(cstmt, 3);
                if (cname != nullptr) {
                    cat.append_column(
                        "main",
                        rel_name,
                        core::SqlCatalogColumn{
                            .name     = cname,
                            .type     = ctype ? ctype : "ANY",
                            .nullable = (notnull == 0),
                        });
                }
            }
            sqlite3_finalize(cstmt);
        }
    }

    return cat;
}

} // namespace capybaradb::drivers
