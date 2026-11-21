#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include "core/config/utf_convert.hpp"
#include "drivers/common/driver_registry.hpp"
#include "drivers/mssql/mssql_catalog_provider.hpp"
#include "drivers/mssql/mssql_driver.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace capybaradb::drivers {
namespace {

using SqlWString = std::basic_string<SQLWCHAR>;

SqlWString to_sqlw(std::string_view utf8) {
    const std::u16string u16 = core::utf8_to_utf16(utf8);
    SqlWString out(u16.size(), static_cast<SQLWCHAR>(0));
    for (size_t i = 0; i < u16.size(); ++i) {
        out[i] = static_cast<SQLWCHAR>(u16[i]);
    }
    return out;
}

std::string from_sqlw(const SQLWCHAR* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return {};
    }
    std::u16string u16(len, u'\0');
    for (size_t i = 0; i < len; ++i) {
        u16[i] = static_cast<char16_t>(buf[i]);
    }
    return core::utf16_to_utf8(u16);
}

std::string quote_tsql_ident(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 4);
    out.push_back('[');
    for (const char ch : ident) {
        if (ch == ']') {
            out.push_back(']');
        }
        out.push_back(ch);
    }
    out.push_back(']');
    return out;
}

std::string quote_tsql_nvarchar(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 5);
    out += "N'";
    for (const char ch : value) {
        if (ch == '\'') {
            out.push_back('\'');
        }
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

DbError stmt_diag(SQLHSTMT hstmt) {
    DbError err;
    SQLWCHAR state[6]{};
    SQLINTEGER native = 0;
    SQLWCHAR msg[1024]{};
    SQLSMALLINT len = 0;
    if (SQL_SUCCEEDED(SQLGetDiagRecW(
            SQL_HANDLE_STMT,
            hstmt,
            1,
            state,
            &native,
            msg,
            static_cast<SQLSMALLINT>(std::size(msg)),
            &len))) {
        err.sql_state   = from_sqlw(state, 5);
        err.native_code = static_cast<int32_t>(native);
        const size_t chars = (len > 0 && static_cast<size_t>(len) < std::size(msg))
            ? static_cast<size_t>(len)
            : 0;
        err.message = from_sqlw(msg, chars);
    } else {
        err.message = "SQL Server catalog query failed.";
    }
    return err;
}

struct StmtGuard final {
    SQLHSTMT hstmt = SQL_NULL_HSTMT;
    explicit StmtGuard(SQLHDBC hdbc) {
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
    StmtGuard(StmtGuard&& other) noexcept : hstmt(other.hstmt) {
        other.hstmt = SQL_NULL_HSTMT;
    }
    StmtGuard& operator=(StmtGuard&& other) noexcept {
        if (this != &other) {
            if (hstmt != SQL_NULL_HSTMT) {
                SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
            }
            hstmt = other.hstmt;
            other.hstmt = SQL_NULL_HSTMT;
        }
        return *this;
    }
    ~StmtGuard() {
        if (hstmt != SQL_NULL_HSTMT) {
            SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        }
    }
};

struct RawOdbcConn final {
    SQLHENV henv      = SQL_NULL_HENV;
    SQLHDBC hdbc      = SQL_NULL_HDBC;
    bool    connected = false;

    RawOdbcConn() = default;
    RawOdbcConn(const RawOdbcConn&) = delete;
    RawOdbcConn& operator=(const RawOdbcConn&) = delete;
    RawOdbcConn(RawOdbcConn&& other) noexcept
        : henv(other.henv)
        , hdbc(other.hdbc)
        , connected(other.connected) {
        other.henv      = SQL_NULL_HENV;
        other.hdbc      = SQL_NULL_HDBC;
        other.connected = false;
    }
    RawOdbcConn& operator=(RawOdbcConn&& other) noexcept {
        if (this != &other) {
            close();
            henv            = other.henv;
            hdbc            = other.hdbc;
            connected       = other.connected;
            other.henv      = SQL_NULL_HENV;
            other.hdbc      = SQL_NULL_HDBC;
            other.connected = false;
        }
        return *this;
    }

    ~RawOdbcConn() {
        close();
    }

    void close() noexcept {
        if (hdbc != SQL_NULL_HDBC) {
            if (connected) {
                SQLDisconnect(hdbc);
            }
            SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            hdbc      = SQL_NULL_HDBC;
            connected = false;
        }
        if (henv != SQL_NULL_HENV) {
            SQLFreeHandle(SQL_HANDLE_ENV, henv);
            henv = SQL_NULL_HENV;
        }
    }
};

std::expected<RawOdbcConn, DbError> open_raw(const ConnectionConfig& cfg, core::CancelToken& cancel) {
    if (cancel.is_canceled()) {
        return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
    }
    const std::string drv = detect_mssql_odbc_driver();
    if (drv.empty()) {
        return std::unexpected(DbError{
            .sql_state = "IM002",
            .message = "No Microsoft SQL Server ODBC driver (18 or 17) is installed.",
        });
    }

    RawOdbcConn conn;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &conn.henv))) {
        return std::unexpected(DbError{.message = "Failed to allocate ODBC environment."});
    }
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(conn.henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0))) {
        SQLSetEnvAttr(conn.henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, conn.henv, &conn.hdbc))) {
        return std::unexpected(DbError{.message = "Failed to allocate ODBC connection."});
    }

    const SQLULEN login_timeout = cfg.connect_timeout_sec > 0 ? cfg.connect_timeout_sec : 10;
    SQLSetConnectAttrW(conn.hdbc, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(login_timeout)), 0);

    const std::string cs = MsSqlDriver::build_connection_string(cfg, drv);
    const SqlWString wcs = to_sqlw(cs);
    std::array<SQLWCHAR, 1024> out{};
    SQLSMALLINT out_len = 0;
    const SQLRETURN rc = SQLDriverConnectW(
        conn.hdbc,
        nullptr,
        const_cast<SQLWCHAR*>(wcs.c_str()),
        static_cast<SQLSMALLINT>(wcs.size()),
        out.data(),
        static_cast<SQLSMALLINT>(out.size()),
        &out_len,
        SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
        SQLWCHAR state[6]{};
        SQLINTEGER native = 0;
        SQLWCHAR msg[1024]{};
        SQLSMALLINT len = 0;
        DbError err{.message = "ODBC connection failed."};
        if (SQL_SUCCEEDED(SQLGetDiagRecW(SQL_HANDLE_DBC, conn.hdbc, 1, state, &native, msg, static_cast<SQLSMALLINT>(std::size(msg)), &len))) {
            err.sql_state   = from_sqlw(state, 5);
            err.native_code = static_cast<int32_t>(native);
            err.message     = from_sqlw(msg, len > 0 ? static_cast<size_t>(len) : 0);
        }
        return std::unexpected(std::move(err));
    }
    conn.connected = true;
    return conn;
}

std::string read_col_str(SQLHSTMT hstmt, SQLUSMALLINT col) {
    std::array<SQLWCHAR, 1024> buf{};
    SQLLEN ind = 0;
    if (SQL_SUCCEEDED(SQLGetData(hstmt, col, SQL_C_WCHAR, buf.data(), static_cast<SQLLEN>(buf.size() * sizeof(SQLWCHAR)), &ind)) && ind > 0) {
        const size_t chars = std::min(static_cast<size_t>(ind / sizeof(SQLWCHAR)), buf.size() - 1);
        return from_sqlw(buf.data(), chars);
    }
    return {};
}

uint64_t read_col_u64(SQLHSTMT hstmt, SQLUSMALLINT col) {
    SQLBIGINT val = 0;
    SQLLEN ind = 0;
    if (SQL_SUCCEEDED(SQLGetData(hstmt, col, SQL_C_SBIGINT, &val, sizeof(val), &ind)) && ind != SQL_NULL_DATA) {
        return static_cast<uint64_t>(val);
    }
    return 0;
}

bool read_col_bool(SQLHSTMT hstmt, SQLUSMALLINT col) {
    SQLINTEGER val = 0;
    SQLLEN ind = 0;
    if (SQL_SUCCEEDED(SQLGetData(hstmt, col, SQL_C_SLONG, &val, sizeof(val), &ind)) && ind != SQL_NULL_DATA) {
        return val != 0;
    }
    return false;
}

} // namespace

std::expected<std::vector<CatalogNodeData>, DbError> MsSqlCatalogProvider::list_children(
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
            folder(CatalogKind::Roles, "Logins"),
        };
    case CatalogKind::Database:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Schemas, "Schemas"),
        };
    case CatalogKind::Schema:
        return std::vector<CatalogNodeData>{
            folder(CatalogKind::Tables, "Tables"),
            folder(CatalogKind::Views, "Views"),
            folder(CatalogKind::Sequences, "Sequences"),
            folder(CatalogKind::Functions, "Functions"),
            folder(CatalogKind::Procedures, "Stored Procedures"),
            folder(CatalogKind::Domains, "User-Defined Types"),
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
        };
    default:
        break;
    }

    if (!parent.can_expand()) {
        return std::vector<CatalogNodeData>{};
    }

    const std::string target_db = parent.database.empty() ? base_cfg.database : parent.database;
    auto conn = open_raw(base_cfg.with_database(target_db), cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    StmtGuard stmt(conn->hdbc);
    if (stmt.hstmt == SQL_NULL_HSTMT) {
        return std::unexpected(DbError{.message = "Failed to allocate ODBC statement handle."});
    }

    core::ScopedCancelCallback cancel_guard(cancel, [&stmt]() {
        SQLCancelHandle(SQL_HANDLE_STMT, stmt.hstmt);
    });

    std::string sql;
    CatalogKind child_kind = CatalogKind::Relation;
    const std::string quoted_schema = quote_tsql_nvarchar(
        parent.schema.empty() ? std::string_view{"dbo"} : std::string_view{parent.schema});
    const std::string oid_lit = std::to_string(parent.oid);

    switch (parent.kind) {
    case CatalogKind::Databases:
        sql = "SELECT CAST(database_id AS BIGINT), name, state_desc, "
              "CASE WHEN state <> 0 THEN 1 ELSE 0 END "
              "FROM sys.databases ORDER BY name";
        child_kind = CatalogKind::Database;
        break;
    case CatalogKind::Roles:
        sql = "SELECT CAST(principal_id AS BIGINT), name, type_desc "
              "FROM sys.server_principals "
              "WHERE type COLLATE DATABASE_DEFAULT IN ("
              "'S' COLLATE DATABASE_DEFAULT, 'U' COLLATE DATABASE_DEFAULT, "
              "'G' COLLATE DATABASE_DEFAULT, 'R' COLLATE DATABASE_DEFAULT, "
              "'E' COLLATE DATABASE_DEFAULT, 'X' COLLATE DATABASE_DEFAULT) "
              "ORDER BY name";
        child_kind = CatalogKind::Role;
        break;
    case CatalogKind::Schemas:
        sql = "SELECT CAST(schema_id AS BIGINT), name FROM sys.schemas "
              "WHERE name COLLATE DATABASE_DEFAULT NOT IN ("
              "N'INFORMATION_SCHEMA' COLLATE DATABASE_DEFAULT, "
              "N'sys' COLLATE DATABASE_DEFAULT, "
              "N'guest' COLLATE DATABASE_DEFAULT) "
              "AND name COLLATE DATABASE_DEFAULT NOT LIKE N'db[_]%' COLLATE DATABASE_DEFAULT "
              "ORDER BY name";
        child_kind = CatalogKind::Schema;
        break;
    case CatalogKind::Tables:
        sql = "SELECT CAST(t.object_id AS BIGINT), t.name, 'r', USER_NAME(COALESCE(t.principal_id, s.principal_id)) "
              "FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "AND t.is_ms_shipped = 0 ORDER BY t.name";
        child_kind = CatalogKind::Relation;
        break;
    case CatalogKind::Views:
        sql = "SELECT CAST(v.object_id AS BIGINT), v.name, 'v', USER_NAME(COALESCE(v.principal_id, s.principal_id)) "
              "FROM sys.views v JOIN sys.schemas s ON s.schema_id = v.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "AND v.is_ms_shipped = 0 ORDER BY v.name";
        child_kind = CatalogKind::Relation;
        break;
    case CatalogKind::Sequences:
        sql = "SELECT CAST(sq.object_id AS BIGINT), sq.name, 'S', USER_NAME(COALESCE(sq.principal_id, s.principal_id)) "
              "FROM sys.sequences sq JOIN sys.schemas s ON s.schema_id = sq.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "ORDER BY sq.name";
        child_kind = CatalogKind::Relation;
        break;
    case CatalogKind::Functions:
        sql = "SELECT CAST(o.object_id AS BIGINT), o.name, USER_NAME(COALESCE(o.principal_id, s.principal_id)) "
              "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "AND o.type COLLATE DATABASE_DEFAULT IN ("
              "'FN' COLLATE DATABASE_DEFAULT, 'IF' COLLATE DATABASE_DEFAULT, "
              "'TF' COLLATE DATABASE_DEFAULT, 'FS' COLLATE DATABASE_DEFAULT, "
              "'FT' COLLATE DATABASE_DEFAULT) ORDER BY o.name";
        child_kind = CatalogKind::Routine;
        break;
    case CatalogKind::Procedures:
        sql = "SELECT CAST(o.object_id AS BIGINT), o.name, USER_NAME(COALESCE(o.principal_id, s.principal_id)) "
              "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "AND o.type COLLATE DATABASE_DEFAULT IN ('P' COLLATE DATABASE_DEFAULT, 'PC' COLLATE DATABASE_DEFAULT) "
              "ORDER BY o.name";
        child_kind = CatalogKind::Routine;
        break;
    case CatalogKind::Domains:
        sql = "SELECT CAST(t.user_type_id AS BIGINT), t.name, 'd' "
              "FROM sys.types t JOIN sys.schemas s ON s.schema_id = t.schema_id "
              "WHERE s.name COLLATE DATABASE_DEFAULT = " + quoted_schema + " COLLATE DATABASE_DEFAULT "
              "AND t.is_user_defined = 1 ORDER BY t.name";
        child_kind = CatalogKind::DataType;
        break;
    case CatalogKind::Columns:
        sql = "SELECT CAST(c.column_id AS BIGINT), c.name, "
              "COALESCE(TYPE_NAME(c.user_type_id), TYPE_NAME(c.system_type_id)), "
              "CASE WHEN c.is_nullable = 1 THEN 1 ELSE 0 END "
              "FROM sys.columns c WHERE c.object_id = " + oid_lit + " ORDER BY c.column_id";
        child_kind = CatalogKind::Column;
        break;
    case CatalogKind::Keys:
        sql = "SELECT CAST(object_id AS BIGINT), name FROM ("
              "SELECT object_id, name COLLATE DATABASE_DEFAULT AS name, 0 AS ord "
              "FROM sys.key_constraints WHERE parent_object_id = " + oid_lit + " "
              "UNION ALL "
              "SELECT object_id, name COLLATE DATABASE_DEFAULT AS name, 1 AS ord "
              "FROM sys.foreign_keys WHERE parent_object_id = " + oid_lit +
              ") k ORDER BY ord, name";
        child_kind = CatalogKind::Key;
        break;
    case CatalogKind::Constraints:
        sql = "SELECT CAST(object_id AS BIGINT), name FROM ("
              "SELECT object_id, name COLLATE DATABASE_DEFAULT AS name "
              "FROM sys.check_constraints WHERE parent_object_id = " + oid_lit + " "
              "UNION ALL "
              "SELECT object_id, name COLLATE DATABASE_DEFAULT AS name "
              "FROM sys.default_constraints WHERE parent_object_id = " + oid_lit +
              ") c ORDER BY name";
        child_kind = CatalogKind::Constraint;
        break;
    case CatalogKind::Triggers:
        sql = "SELECT CAST(object_id AS BIGINT), name FROM sys.triggers "
              "WHERE parent_id = " + oid_lit + " ORDER BY name";
        child_kind = CatalogKind::Trigger;
        break;
    case CatalogKind::Indexes:
        sql = "SELECT CAST(index_id AS BIGINT), name FROM sys.indexes "
              "WHERE object_id = " + oid_lit + " AND index_id > 0 AND name IS NOT NULL ORDER BY name";
        child_kind = CatalogKind::Index;
        break;
    default:
        return std::vector<CatalogNodeData>{};
    }

    const SqlWString wsql = to_sqlw(sql);
    if (!SQL_SUCCEEDED(SQLExecDirectW(stmt.hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLINTEGER>(wsql.size())))) {
        return std::unexpected(stmt_diag(stmt.hstmt));
    }

    std::vector<CatalogNodeData> out;
    while (SQL_SUCCEEDED(SQLFetch(stmt.hstmt))) {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "Operation canceled.", .is_canceled = true});
        }
        CatalogNodeData item;
        item.kind = child_kind;
        item.oid  = read_col_u64(stmt.hstmt, 1);

        if (child_kind == CatalogKind::Database) {
            const std::string raw_name   = read_col_str(stmt.hstmt, 2);
            const std::string state_desc = read_col_str(stmt.hstmt, 3);
            const bool        offline    = read_col_bool(stmt.hstmt, 4);
            item.database                = raw_name;
            item.is_offline              = offline;
            item.name                    = (offline && !state_desc.empty())
                ? (raw_name + " (" + state_desc + ")")
                : raw_name;
        } else if (child_kind == CatalogKind::Role) {
            const std::string raw_name  = read_col_str(stmt.hstmt, 2);
            const std::string type_desc = read_col_str(stmt.hstmt, 3);
            item.database               = target_db;
            item.role_name              = raw_name;
            item.name                   = type_desc.empty() ? raw_name : (raw_name + " (" + type_desc + ")");
        } else if (child_kind == CatalogKind::Column) {
            const std::string col_name  = read_col_str(stmt.hstmt, 2);
            const std::string type_name = read_col_str(stmt.hstmt, 3);
            const bool        nullable  = read_col_bool(stmt.hstmt, 4);
            item.database               = target_db;
            item.schema                 = parent.schema;
            item.name                   = col_name + " (" + (type_name.empty() ? std::string{"unknown"} : type_name) +
                                          (nullable ? ", null)" : ", not null)");
        } else {
            item.name     = read_col_str(stmt.hstmt, 2);
            item.database = target_db;
            if (child_kind == CatalogKind::Schema) {
                item.schema = item.name;
            } else {
                item.schema = parent.schema;
            }

            if (child_kind == CatalogKind::Relation) {
                const std::string rt = read_col_str(stmt.hstmt, 3);
                item.relation_type = rt.empty() ? '\0' : rt[0];
                item.owner         = read_col_str(stmt.hstmt, 4);
            } else if (child_kind == CatalogKind::DataType) {
                const std::string dtk = read_col_str(stmt.hstmt, 3);
                item.data_type_kind = dtk.empty() ? '\0' : dtk[0];
            } else if (child_kind == CatalogKind::Routine) {
                if (parent.kind == CatalogKind::Functions) {
                    item.name += "()";
                }
                item.routine_kind = (parent.kind == CatalogKind::Procedures) ? 'p' : 'f';
                item.owner        = read_col_str(stmt.hstmt, 3);
            }
        }

        out.push_back(std::move(item));
    }
    return out;
}

std::expected<std::vector<std::string>, DbError> MsSqlCatalogProvider::list_databases(
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

std::expected<std::vector<std::string>, DbError> MsSqlCatalogProvider::read_table_columns(
    const ConnectionConfig& base_cfg,
    std::string_view database,
    std::string_view schema,
    std::string_view table,
    core::CancelToken& cancel) {
    const ConnectionConfig cfg = database.empty() ? base_cfg : base_cfg.with_database(std::string(database));
    auto conn = open_raw(cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    StmtGuard stmt(conn->hdbc);
    if (stmt.hstmt == SQL_NULL_HSTMT) {
        return std::unexpected(DbError{.message = "Failed to allocate ODBC statement."});
    }

    const std::string sql =
        "SELECT c.name FROM sys.columns c "
        "JOIN sys.objects o ON o.object_id = c.object_id "
        "JOIN sys.schemas s ON s.schema_id = o.schema_id "
        "WHERE s.name COLLATE DATABASE_DEFAULT = " + quote_tsql_nvarchar(schema) + " COLLATE DATABASE_DEFAULT "
        "AND o.name COLLATE DATABASE_DEFAULT = " + quote_tsql_nvarchar(table) + " COLLATE DATABASE_DEFAULT "
        "ORDER BY c.column_id";
    const SqlWString wsql = to_sqlw(sql);
    if (!SQL_SUCCEEDED(SQLExecDirectW(stmt.hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLINTEGER>(wsql.size())))) {
        return std::unexpected(stmt_diag(stmt.hstmt));
    }

    std::vector<std::string> cols;
    while (SQL_SUCCEEDED(SQLFetch(stmt.hstmt))) {
        cols.push_back(read_col_str(stmt.hstmt, 1));
    }
    return cols;
}

std::string MsSqlCatalogProvider::generate_select_top_sql(
    std::string_view schema,
    std::string_view object,
    std::span<const std::string> columns,
    uint32_t top_n) const {
    const uint32_t limit = top_n > 0 ? top_n : 1000u;
    std::string sql = "SELECT TOP (";
    sql += std::to_string(limit);
    sql += ") ";
    if (columns.empty()) {
        sql += "*";
    } else {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                sql += "\n      ,";
            }
            sql += quote_tsql_ident(columns[i]);
        }
    }
    sql += "\n  FROM ";
    sql += quote_tsql_ident(schema);
    sql += ".";
    sql += quote_tsql_ident(object);
    sql += ";\n";
    return sql;
}

std::expected<core::SqlCompletionCatalog, DbError> MsSqlCatalogProvider::load_completion_catalog(
    const ConnectionConfig& base_cfg,
    core::CancelToken& cancel) {
    auto conn = open_raw(base_cfg, cancel);
    if (!conn) {
        return std::unexpected(conn.error());
    }

    core::SqlCompletionCatalog cat;
    cat.is_tsql  = true;
    cat.database = base_cfg.database;

    auto run_query = [&](std::string_view query_sql, auto&& on_row) -> std::expected<void, DbError> {
        if (cancel.is_canceled()) {
            return std::unexpected(DbError{.message = "IntelliSense catalog load canceled.", .is_canceled = true});
        }
        StmtGuard stmt(conn->hdbc);
        if (stmt.hstmt == SQL_NULL_HSTMT) {
            return std::unexpected(DbError{.message = "Failed to allocate ODBC statement."});
        }
        const SqlWString wsql = to_sqlw(query_sql);
        if (!SQL_SUCCEEDED(SQLExecDirectW(stmt.hstmt, const_cast<SQLWCHAR*>(wsql.c_str()), static_cast<SQLINTEGER>(wsql.size())))) {
            return std::unexpected(stmt_diag(stmt.hstmt));
        }
        while (SQL_SUCCEEDED(SQLFetch(stmt.hstmt))) {
            if (cancel.is_canceled()) {
                return std::unexpected(DbError{.message = "IntelliSense catalog load canceled.", .is_canceled = true});
            }
            on_row(stmt.hstmt);
        }
        return {};
    };

    // 1. Default schema -> search_path
    if (auto r = run_query("SELECT ISNULL(SCHEMA_NAME(), N'dbo')", [&](SQLHSTMT st) {
            const std::string def_sch = read_col_str(st, 1);
            if (!def_sch.empty()) cat.search_path.push_back(def_sch);
        }); !r) {
        return std::unexpected(r.error());
    }
    if (std::find(cat.search_path.begin(), cat.search_path.end(), "dbo") == cat.search_path.end()) {
        cat.search_path.emplace_back("dbo");
    }

    // 2. Schemas
    if (auto r = run_query("SELECT name FROM sys.schemas ORDER BY name", [&](SQLHSTMT st) {
            cat.schemas.push_back(read_col_str(st, 1));
        }); !r) {
        return std::unexpected(r.error());
    }

    // 3. Tables & Views
    {
        const std::string sql =
            "SELECT TOP (" + std::to_string(core::SqlCompletionCatalog::kMaxRelations) + ") "
            "s.name, o.name, CASE WHEN o.type = 'V' THEN 1 ELSE 0 END "
            "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
            "WHERE o.type IN ('U','V') AND o.is_ms_shipped = 0 "
            "ORDER BY s.name, o.name";
        if (auto r = run_query(sql, [&](SQLHSTMT st) {
                cat.relations.push_back(core::SqlCatalogRelation{
                    .schema  = read_col_str(st, 1),
                    .name    = read_col_str(st, 2),
                    .is_view = (read_col_str(st, 3) == "1"),
                });
            }); !r) {
            return std::unexpected(r.error());
        }
    }

    // 4. Columns
    {
        const std::string sql =
            "SELECT TOP (" + std::to_string(core::SqlCompletionCatalog::kMaxColumns) + ") "
            "s.name, o.name, c.name, t.name, CASE WHEN c.is_nullable = 1 THEN 1 ELSE 0 END "
            "FROM sys.columns c "
            "JOIN sys.objects o ON o.object_id = c.object_id "
            "JOIN sys.schemas s ON s.schema_id = o.schema_id "
            "JOIN sys.types t ON t.user_type_id = c.user_type_id "
            "WHERE o.type IN ('U','V') AND o.is_ms_shipped = 0 "
            "ORDER BY s.name, o.name, c.column_id";
        if (auto r = run_query(sql, [&](SQLHSTMT st) {
                cat.append_column(
                    read_col_str(st, 1),
                    read_col_str(st, 2),
                    core::SqlCatalogColumn{
                        .name     = read_col_str(st, 3),
                        .type     = read_col_str(st, 4),
                        .nullable = (read_col_str(st, 5) == "1"),
                    });
            }); !r) {
            return std::unexpected(r.error());
        }
    }

    // 5. Functions
    {
        const std::string sql =
            "SELECT TOP (" + std::to_string(core::SqlCompletionCatalog::kMaxFunctions) + ") "
            "s.name, o.name, CASE WHEN o.type IN ('IF','TF') THEN 1 ELSE 0 END "
            "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
            "WHERE o.type IN ('FN','IF','TF') AND o.is_ms_shipped = 0 "
            "ORDER BY s.name, o.name";
        if (auto r = run_query(sql, [&](SQLHSTMT st) {
                const bool ret_rows = (read_col_str(st, 3) == "1");
                cat.functions.push_back(core::SqlCatalogFunction{
                    .schema       = read_col_str(st, 1),
                    .name         = read_col_str(st, 2),
                    .arguments    = {},
                    .result       = ret_rows ? "table" : "scalar",
                    .returns_rows = ret_rows,
                });
            }); !r) {
            return std::unexpected(r.error());
        }
    }

    // 6. Foreign keys
    {
        const std::string sql =
            "SELECT TOP (" + std::to_string(core::SqlCompletionCatalog::kMaxForeignKeys) + ") "
            "fk.name, ps.name, pt.name, pc.name, rs.name, rt.name, rc.name "
            "FROM sys.foreign_keys fk "
            "JOIN sys.foreign_key_columns fkc ON fkc.constraint_object_id = fk.object_id "
            "JOIN sys.tables pt ON pt.object_id = fkc.parent_object_id "
            "JOIN sys.schemas ps ON ps.schema_id = pt.schema_id "
            "JOIN sys.columns pc ON pc.object_id = fkc.parent_object_id AND pc.column_id = fkc.parent_column_id "
            "JOIN sys.tables rt ON rt.object_id = fkc.referenced_object_id "
            "JOIN sys.schemas rs ON rs.schema_id = rt.schema_id "
            "JOIN sys.columns rc ON rc.object_id = fkc.referenced_object_id AND rc.column_id = fkc.referenced_column_id "
            "ORDER BY fk.object_id, fkc.constraint_column_id";
        if (auto r = run_query(sql, [&](SQLHSTMT st) {
                const std::string fk_name = read_col_str(st, 1);
                const std::string ps = read_col_str(st, 2);
                const std::string pt = read_col_str(st, 3);
                const std::string pc = read_col_str(st, 4);
                const std::string rs = read_col_str(st, 5);
                const std::string rt = read_col_str(st, 6);
                const std::string rc = read_col_str(st, 7);
                if (!cat.foreign_keys.empty() &&
                    cat.foreign_keys.back().name == fk_name &&
                    cat.foreign_keys.back().schema == ps &&
                    cat.foreign_keys.back().table == pt) {
                    cat.foreign_keys.back().columns.push_back(pc);
                    cat.foreign_keys.back().target_columns.push_back(rc);
                } else {
                    cat.foreign_keys.push_back(core::SqlCatalogForeignKey{
                        .name           = fk_name,
                        .schema         = ps,
                        .table          = pt,
                        .columns        = {pc},
                        .target_schema  = rs,
                        .target_table   = rt,
                        .target_columns = {rc},
                    });
                }
            }); !r) {
            return std::unexpected(r.error());
        }
    }

    return cat;
}

} // namespace capybaradb::drivers
