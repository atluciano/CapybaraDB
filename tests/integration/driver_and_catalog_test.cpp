#include "core/threading/cancel_token.hpp"
#include "core/threading/stream_sink.hpp"
#include "dialects/sql_dialect.hpp"
#include "drivers/common/driver_registry.hpp"
#include "drivers/mssql/mssql_driver.hpp"
#include "drivers/postgres/pg_type_oids.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <vector>

TEST_CASE("DriverRegistry instantiates PostgreSQL and SQL Server drivers and catalog providers and enumerates ODBC drivers", "[integration][drivers]") {
    auto pg_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::PostgreSql);
    auto ms_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::SqlServer);
    auto pg_cat = capybaradb::drivers::create_catalog_provider(capybaradb::drivers::EngineKind::PostgreSql);
    auto ms_cat = capybaradb::drivers::create_catalog_provider(capybaradb::drivers::EngineKind::SqlServer);

    REQUIRE(pg_drv != nullptr);
    REQUIRE(ms_drv != nullptr);
    REQUIRE(pg_cat != nullptr);
    REQUIRE(ms_cat != nullptr);
    REQUIRE_FALSE(pg_drv->is_connected());
    REQUIRE_FALSE(ms_drv->is_connected());

    // Enumerate installed ODBC drivers cleanly without throwing
    const auto drivers = capybaradb::drivers::installed_odbc_drivers();
    const std::string detected = capybaradb::drivers::detect_mssql_odbc_driver();
    if (!detected.empty()) {
        REQUIRE(capybaradb::drivers::is_mssql_odbc_available());
    }
    (void)drivers;
}

TEST_CASE("PostgreSQL static OID table classifies types and MsSqlDriver formats escaped ODBC connection strings", "[integration][drivers]") {
    REQUIRE(capybaradb::drivers::lookup_pg_oid(25u).name == "text");
    REQUIRE(capybaradb::drivers::lookup_pg_oid(25u).may_hold_json);
    REQUIRE(capybaradb::drivers::lookup_pg_oid(3802u).kind == capybaradb::drivers::PgLogicalKind::Json);
    REQUIRE(capybaradb::drivers::lookup_pg_oid(1700u).kind == capybaradb::drivers::PgLogicalKind::Numeric);

    capybaradb::drivers::ConnectionConfig cfg;
    cfg.engine   = capybaradb::drivers::EngineKind::SqlServer;
    cfg.host     = "sql.synthetic.local";
    cfg.port     = 1433;
    cfg.database = "app_db";
    cfg.username = "sa";
    cfg.password = "P@ss;with{brace}";
    cfg.auth     = capybaradb::drivers::AuthKind::Password;
    cfg.tls_mode = capybaradb::drivers::TlsMode::VerifyFull;

    const std::string cs18 = capybaradb::drivers::MsSqlDriver::build_connection_string(cfg, "ODBC Driver 18 for SQL Server");
    REQUIRE(cs18.find("Driver={ODBC Driver 18 for SQL Server}") != std::string::npos);
    REQUIRE(cs18.find("Server=tcp:sql.synthetic.local,1433") != std::string::npos);
    REQUIRE(cs18.find("Encrypt=Strict;") != std::string::npos);
    REQUIRE(cs18.find("PWD={P@ss;with{brace}}};") != std::string::npos);

    cfg.auth     = capybaradb::drivers::AuthKind::IntegratedWindowsOrKerberos;
    cfg.tls_mode = capybaradb::drivers::TlsMode::Prefer;
    const std::string cs17 = capybaradb::drivers::MsSqlDriver::build_connection_string(cfg, "ODBC Driver 17 for SQL Server");
    REQUIRE(cs17.find("Trusted_Connection=Yes;") != std::string::npos);
    REQUIRE(cs17.find("Encrypt=Yes;TrustServerCertificate=Yes;") != std::string::npos);
}

TEST_CASE("CatalogProvider implementations synthesize root folders and quote identifiers in SELECT TOP scripts", "[integration][catalog]") {
    capybaradb::drivers::ConnectionConfig cfg;
    capybaradb::core::CancelToken cancel;

    auto pg_cat = capybaradb::drivers::create_catalog_provider(capybaradb::drivers::EngineKind::PostgreSql);
    const auto pg_server = capybaradb::drivers::CatalogNodeData::make_folder(capybaradb::drivers::CatalogKind::Server, "root", "postgres");
    const auto pg_root_children = pg_cat->list_children(cfg, pg_server, cancel);
    REQUIRE(pg_root_children.has_value());
    REQUIRE(pg_root_children->size() == 2);
    REQUIRE((*pg_root_children)[0].kind == capybaradb::drivers::CatalogKind::Databases);
    REQUIRE((*pg_root_children)[1].kind == capybaradb::drivers::CatalogKind::Roles);

    const std::vector<std::string> cols = {"id", "display\"name"};
    const std::string pg_sql = pg_cat->generate_select_top_sql("public", "my\"table", cols, 500);
    REQUIRE(pg_sql.find("SELECT \"id\"") != std::string::npos);
    REQUIRE(pg_sql.find("\"display\"\"name\"") != std::string::npos);
    REQUIRE(pg_sql.find("FROM \"public\".\"my\"\"table\"") != std::string::npos);
    REQUIRE(pg_sql.find("LIMIT 500") != std::string::npos);

    auto ms_cat = capybaradb::drivers::create_catalog_provider(capybaradb::drivers::EngineKind::SqlServer);
    const auto ms_server = capybaradb::drivers::CatalogNodeData::make_folder(capybaradb::drivers::CatalogKind::Server, "root", "master");
    const auto ms_root_children = ms_cat->list_children(cfg, ms_server, cancel);
    REQUIRE(ms_root_children.has_value());
    REQUIRE(ms_root_children->size() == 2);

    const std::vector<std::string> ms_cols = {"Id", "Col]1"};
    const std::string ms_sql = ms_cat->generate_select_top_sql("dbo", "Ord]ers", ms_cols, 200);
    REQUIRE(ms_sql.find("SELECT TOP (200) [Id]") != std::string::npos);
    REQUIRE(ms_sql.find("[Col]]1]") != std::string::npos);
    REQUIRE(ms_sql.find("FROM [dbo].[Ord]]ers];") != std::string::npos);
}

TEST_CASE("DbDriver::test_connection aborts immediately when passed a pre-canceled CancelToken", "[integration][drivers]") {
    capybaradb::drivers::ConnectionConfig pg_cfg;
    pg_cfg.host     = "127.0.0.1";
    pg_cfg.port     = 59999;
    pg_cfg.database = "postgres";
    pg_cfg.username = "postgres";

    capybaradb::core::CancelToken cancel;
    cancel.cancel();

    auto pg_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::PostgreSql);
    const auto pg_res = pg_drv->test_connection(pg_cfg, cancel);
    REQUIRE_FALSE(pg_res.has_value());
    REQUIRE(pg_res.error().is_canceled);

    auto ms_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::SqlServer);
    capybaradb::drivers::ConnectionConfig ms_cfg = pg_cfg;
    ms_cfg.engine = capybaradb::drivers::EngineKind::SqlServer;
    const auto ms_res = ms_drv->test_connection(ms_cfg, cancel);
    REQUIRE_FALSE(ms_res.has_value());
    REQUIRE(ms_res.error().is_canceled);
}

TEST_CASE("DbError::format_detailed formats PostgreSQL and SQL Server diagnostics and truncates oversized messages safely", "[integration][drivers]") {
    capybaradb::drivers::DbError pg_err;
    pg_err.severity  = "ERROR";
    pg_err.sql_state = "42P01";
    pg_err.message   = "relation \"missing\" does not exist";
    pg_err.position  = 15;
    pg_err.schema    = "public";
    pg_err.table     = "missing";
    pg_err.hint      = "Check table name";

    const std::string pg_fmt = pg_err.format_detailed(capybaradb::drivers::EngineKind::PostgreSql);
    REQUIRE(pg_fmt.find("SQLSTATE: 42P01") != std::string::npos);
    REQUIRE(pg_fmt.find("Severity: ERROR") != std::string::npos);
    REQUIRE(pg_fmt.find("Server character position: 15") != std::string::npos);
    REQUIRE(pg_fmt.find("Hint: Check table name") != std::string::npos);
    REQUIRE(pg_fmt.find("Schema: public") != std::string::npos);

    capybaradb::drivers::DbError ms_err;
    ms_err.native_code = 208;
    ms_err.sql_state   = "42S02";
    ms_err.message     = "Invalid object name 'missing'";
    ms_err.position    = 2;
    const std::string ms_fmt = ms_err.format_detailed(capybaradb::drivers::EngineKind::SqlServer);
    REQUIRE(ms_fmt.find("Msg 208") != std::string::npos);
    REQUIRE(ms_fmt.find("Line 2") != std::string::npos);
    REQUIRE(ms_fmt.find("42S02") != std::string::npos);

    capybaradb::drivers::DbError huge;
    huge.message = std::string(5000, 'A') + "\xC3\xA9";
    const std::string huge_fmt = huge.format_detailed(capybaradb::drivers::EngineKind::PostgreSql);
    REQUIRE(huge_fmt.size() <= 4150);
    REQUIRE(huge_fmt.find("[truncated]") != std::string::npos);
}

TEST_CASE("DbDriver::execute_stream and parse_only fail cleanly when disconnected or pre-canceled", "[integration][drivers]") {
    capybaradb::core::CancelToken active_cancel;
    capybaradb::core::QueryExecutionResult result;
    capybaradb::core::StreamPublishSink sink;
    capybaradb::drivers::ExecOptions opts;

    auto pg_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::PostgreSql);
    const auto pg_exec_disconn = pg_drv->execute_stream("SELECT 1", opts, result, sink, active_cancel);
    REQUIRE_FALSE(pg_exec_disconn.has_value());
    REQUIRE_FALSE(pg_exec_disconn.error().message.empty());
    const auto pg_parse_disconn = pg_drv->parse_only("SELECT 1", active_cancel);
    REQUIRE_FALSE(pg_parse_disconn.has_value());

    auto ms_drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::SqlServer);
    const auto ms_exec_disconn = ms_drv->execute_stream("SELECT 1", opts, result, sink, active_cancel);
    REQUIRE_FALSE(ms_exec_disconn.has_value());
    REQUIRE_FALSE(ms_exec_disconn.error().message.empty());
    const auto ms_parse_disconn = ms_drv->parse_only("SELECT 1", active_cancel);
    REQUIRE_FALSE(ms_parse_disconn.has_value());

    capybaradb::core::CancelToken pre_cancel;
    pre_cancel.cancel();
    const auto pg_precancel = pg_drv->execute_stream("SELECT 1", opts, result, sink, pre_cancel);
    REQUIRE_FALSE(pg_precancel.has_value());
    REQUIRE(pg_precancel.error().is_canceled);
    const auto ms_precancel = ms_drv->parse_only("SELECT 1", pre_cancel);
    REQUIRE_FALSE(ms_precancel.has_value());
    REQUIRE(ms_precancel.error().is_canceled);
}

TEST_CASE("SqliteDriver executes DDL, DML, and streaming SELECTs and SqliteCatalogProvider introspects in-memory databases", "[integration][sqlite]") {
    capybaradb::drivers::ConnectionConfig cfg;
    cfg.engine   = capybaradb::drivers::EngineKind::Sqlite;
    cfg.database = ":memory:";

    auto drv = capybaradb::drivers::create_db_driver(capybaradb::drivers::EngineKind::Sqlite);
    auto cat = capybaradb::drivers::create_catalog_provider(capybaradb::drivers::EngineKind::Sqlite);
    REQUIRE(drv != nullptr);
    REQUIRE(cat != nullptr);

    capybaradb::core::CancelToken cancel;
    const auto conn_res = drv->connect(cfg, cancel);
    REQUIRE(conn_res.has_value());
    REQUIRE(drv->is_connected());

    const auto sess = drv->session_info();
    REQUIRE(sess.engine == capybaradb::drivers::EngineKind::Sqlite);
    REQUIRE(sess.database == ":memory:");

    capybaradb::core::QueryExecutionResult result;
    capybaradb::core::StreamPublishSink sink;
    capybaradb::drivers::ExecOptions opts;

    // 1. DDL: Create table
    const auto create_res = drv->execute_stream(
        "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL, data BLOB);",
        opts, result, sink, cancel);
    REQUIRE(create_res.has_value());

    // 2. DML: Insert records
    const auto insert_res = drv->execute_stream(
        "INSERT INTO items (id, name, price, data) VALUES (1, 'Widget', 19.99, X'CAFE'), (2, 'Gadget', NULL, NULL);",
        opts, result, sink, cancel);
    REQUIRE(insert_res.has_value());
    REQUIRE(result.total_rows_affected.load() == 2);

    // 3. Query: SELECT
    capybaradb::core::QueryExecutionResult q_result;
    const auto query_res = drv->execute_stream(
        "SELECT id, name, price, data FROM items ORDER BY id;",
        opts, q_result, sink, cancel);
    REQUIRE(query_res.has_value());
    REQUIRE(q_result.result_set_count.load() == 1);

    const auto* rs = q_result.result_sets[0].load();
    REQUIRE(rs != nullptr);
    REQUIRE(rs->col_count == 4);
    REQUIRE(rs->published_rows.load() == 2);

    // Inspect rows
    const auto loc0 = rs->locate_row(0);
    const auto* chunk0 = loc0.first;
    const uint32_t r0  = loc0.second;
    REQUIRE(chunk0 != nullptr);
    REQUIRE(chunk0->text_of(chunk0->cell(r0, 0)) == "1");
    REQUIRE(chunk0->text_of(chunk0->cell(r0, 1)) == "Widget");
    REQUIRE(chunk0->text_of(chunk0->cell(r0, 2)).find("19.99") != std::string_view::npos);
    REQUIRE(chunk0->text_of(chunk0->cell(r0, 3)).find("CAFE") != std::string_view::npos);

    const auto loc1 = rs->locate_row(1);
    const auto* chunk1 = loc1.first;
    const uint32_t r1  = loc1.second;
    REQUIRE(chunk1 != nullptr);
    REQUIRE(chunk1->text_of(chunk1->cell(r1, 0)) == "2");
    REQUIRE(chunk1->text_of(chunk1->cell(r1, 1)) == "Gadget");
    REQUIRE(chunk1->cell(r1, 2).is_null());

    // 4. Catalog Provider test
    const auto dbs = cat->list_databases(cfg, cancel);
    REQUIRE(dbs.has_value());
    REQUIRE_FALSE(dbs->empty());

    const auto root = capybaradb::drivers::CatalogNodeData::make_folder(capybaradb::drivers::CatalogKind::Server, "root");
    const auto root_kids = cat->list_children(cfg, root, cancel);
    REQUIRE(root_kids.has_value());
    REQUIRE((*root_kids)[0].kind == capybaradb::drivers::CatalogKind::Databases);

    const auto db_node = capybaradb::drivers::CatalogNodeData::make_folder(capybaradb::drivers::CatalogKind::Database, "main", "main");
    const auto db_kids = cat->list_children(cfg, db_node, cancel);
    REQUIRE(db_kids.has_value());
    REQUIRE(db_kids->size() == 2); // Tables, Views

    // 5. Select Top SQL generation
    const std::vector<std::string> cols = {"id", "name"};
    const std::string top_sql = cat->generate_select_top_sql("main", "items", cols, 100);
    REQUIRE(top_sql.find("SELECT \"id\", \"name\"") != std::string::npos);
    REQUIRE(top_sql.find("FROM \"items\"") != std::string::npos);
    REQUIRE(top_sql.find("LIMIT 100") != std::string::npos);

    // 6. Dialect keyword check
    const auto sq_eng = capybaradb::drivers::EngineKind::Sqlite;
    REQUIRE(capybaradb::dialects::is_sql_keyword(sq_eng, "select"));
    REQUIRE(capybaradb::dialects::is_sql_keyword(sq_eng, "autoincrement"));
    REQUIRE(capybaradb::dialects::is_sql_keyword(sq_eng, "pragma"));
    REQUIRE_FALSE(capybaradb::dialects::is_sql_keyword(sq_eng, "nonexistent_word"));

    drv->disconnect();
    REQUIRE_FALSE(drv->is_connected());
}

TEST_CASE("DriverRegistry lazy-loads driver plugin DLLs on demand and survives repeated load-execute-unload cycles", "[integration][plugins]") {
    using namespace capybaradb::drivers;

    // Ensure clean starting state with all unused plugins unloaded
    (void)unload_unused_driver_modules();
    REQUIRE_FALSE(is_driver_module_loaded(EngineKind::PostgreSql));
    REQUIRE_FALSE(is_driver_module_loaded(EngineKind::SqlServer));
    REQUIRE_FALSE(is_driver_module_loaded(EngineKind::Sqlite));

    // Enumerating installed ODBC drivers must NOT load the MSSQL plugin DLL
    (void)installed_odbc_drivers();
    (void)is_mssql_odbc_available();
    REQUIRE_FALSE(is_driver_module_loaded(EngineKind::SqlServer));

    // Repeat 3x full Load -> Use -> Destroy -> Unload -> Reload cycles across all 3 engines
    for (int cycle = 0; cycle < 3; ++cycle) {
        // 1. SQLite full cycle: Load -> Connect(:memory:) -> DDL/DML/SELECT -> Catalog -> Disconnect -> Unload
        {
            auto sq_drv = create_db_driver(EngineKind::Sqlite);
            auto sq_cat = create_catalog_provider(EngineKind::Sqlite);
            REQUIRE(sq_drv != nullptr);
            REQUIRE(sq_cat != nullptr);
            REQUIRE(is_driver_module_loaded(EngineKind::Sqlite));
            REQUIRE(active_driver_instance_count(EngineKind::Sqlite) == 2);
            REQUIRE_FALSE(is_driver_module_loaded(EngineKind::PostgreSql));
            REQUIRE_FALSE(is_driver_module_loaded(EngineKind::SqlServer));

            // While instances are alive, unload_unused_driver_modules() must NOT unload SQLite
            REQUIRE(unload_unused_driver_modules() == 0);
            REQUIRE(is_driver_module_loaded(EngineKind::Sqlite));

            ConnectionConfig cfg;
            cfg.engine   = EngineKind::Sqlite;
            cfg.database = ":memory:";
            capybaradb::core::CancelToken cancel;
            REQUIRE(sq_drv->connect(cfg, cancel).has_value());

            capybaradb::core::QueryExecutionResult qres;
            capybaradb::core::StreamPublishSink sink;
            ExecOptions opts;
            REQUIRE(sq_drv->execute_stream(
                "CREATE TABLE cycle_t(x INT); INSERT INTO cycle_t VALUES (42); SELECT x FROM cycle_t;",
                opts, qres, sink, cancel).has_value());
            REQUIRE(qres.result_set_count.load() == 1);
            REQUIRE(qres.result_sets[0].load()->published_rows.load() == 1);

            sq_drv->disconnect();
        }
        REQUIRE(active_driver_instance_count(EngineKind::Sqlite) == 0);
        REQUIRE(unload_unused_driver_modules() == 1);
        REQUIRE_FALSE(is_driver_module_loaded(EngineKind::Sqlite));

        // 2. PostgreSQL full cycle: Load -> Catalog & Pre-canceled connect/query -> Destroy -> Unload
        {
            auto pg_drv = create_db_driver(EngineKind::PostgreSql);
            auto pg_cat = create_catalog_provider(EngineKind::PostgreSql);
            REQUIRE(pg_drv != nullptr);
            REQUIRE(pg_cat != nullptr);
            REQUIRE(is_driver_module_loaded(EngineKind::PostgreSql));
            REQUIRE(active_driver_instance_count(EngineKind::PostgreSql) == 2);

            ConnectionConfig pg_cfg;
            pg_cfg.engine = EngineKind::PostgreSql;
            capybaradb::core::CancelToken cancel;
            const auto root = CatalogNodeData::make_folder(CatalogKind::Server, "root", "postgres");
            REQUIRE(pg_cat->list_children(pg_cfg, root, cancel).has_value());

            cancel.cancel();
            const auto conn_res = pg_drv->test_connection(pg_cfg, cancel);
            REQUIRE_FALSE(conn_res.has_value());
            REQUIRE(conn_res.error().is_canceled);
        }
        REQUIRE(active_driver_instance_count(EngineKind::PostgreSql) == 0);
        REQUIRE(unload_unused_driver_modules() == 1);
        REQUIRE_FALSE(is_driver_module_loaded(EngineKind::PostgreSql));

        // 3. SQL Server full cycle: Load -> Catalog & Pre-canceled connect/query -> Destroy -> Unload
        {
            auto ms_drv = create_db_driver(EngineKind::SqlServer);
            auto ms_cat = create_catalog_provider(EngineKind::SqlServer);
            REQUIRE(ms_drv != nullptr);
            REQUIRE(ms_cat != nullptr);
            REQUIRE(is_driver_module_loaded(EngineKind::SqlServer));
            REQUIRE(active_driver_instance_count(EngineKind::SqlServer) == 2);

            ConnectionConfig ms_cfg;
            ms_cfg.engine = EngineKind::SqlServer;
            capybaradb::core::CancelToken cancel;
            const auto root = CatalogNodeData::make_folder(CatalogKind::Server, "root", "master");
            REQUIRE(ms_cat->list_children(ms_cfg, root, cancel).has_value());

            cancel.cancel();
            const auto conn_res = ms_drv->test_connection(ms_cfg, cancel);
            REQUIRE_FALSE(conn_res.has_value());
            REQUIRE(conn_res.error().is_canceled);
        }
        REQUIRE(active_driver_instance_count(EngineKind::SqlServer) == 0);
        REQUIRE(unload_unused_driver_modules() == 1);
        REQUIRE_FALSE(is_driver_module_loaded(EngineKind::SqlServer));
    }
}
