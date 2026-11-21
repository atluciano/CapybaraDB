#include "core/completion/accent_fold.hpp"
#include "core/completion/sql_completion.hpp"
#include "core/config/connection_profile.hpp"
#include "core/config/environment_options.hpp"
#include "core/config/query_history.hpp"
#include "core/config/query_options.hpp"
#include "core/config/query_recovery.hpp"
#include "core/plan/execution_plan.hpp"
#include "core/result/result_exporter.hpp"
#include "core/result/result_formatter.hpp"
#include "core/result/result_set_storage.hpp"
#include "core/threading/cancel_token.hpp"
#include "drivers/mssql/mssql_type_utils.hpp"
#include "drivers/postgres/pg_type_oids.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

class TempDirGuard final {
public:
    explicit TempDirGuard(const char* name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("capybaradb_m5_" + std::string(name) + "_" + std::to_string(ticks));
        std::filesystem::create_directories(path_);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_all_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("QueryOptions normalizes bounds, EnvironmentOptionsStore persists defaults, and ConnectionProfileStore manages server groups", "[core][config][query_options]") {
    using namespace capybaradb::core;

    STATIC_REQUIRE(std::is_trivially_copyable_v<QueryOptions>);
    QueryOptions q;
    q.execution_timeout_seconds = -10;
    q.lock_timeout_ms           = 999999999;
    q.max_arena_mib             = 4;
    q.text_max_characters       = 99999;
    q.custom_delimiter          = '\n';
    q.plan_warning_ratio        = 1;
    const QueryOptions norm = q.normalized();
    REQUIRE(norm.execution_timeout_seconds == 0);
    REQUIRE(norm.lock_timeout_ms == -1);
    REQUIRE(norm.max_arena_mib == QueryOptions::kDefaultMaxArenaMiB);
    REQUIRE(norm.text_max_characters == TextResultsFormat::kDefaultMaxCharacters);
    REQUIRE(norm.custom_delimiter == ';');
    REQUIRE(norm.plan_warning_ratio == ExecutionPlanWarningOptions::kDefaultRatio);

    TempDirGuard tmp("env_m5");
    const std::filesystem::path env_file = tmp.path() / "environment.json";
    EnvironmentDocument d = load_environment_options(env_file);
    d.default_query_options.execution_timeout_seconds = 45;
    d.default_query_options.nocount                   = true;
    d.default_query_options.isolation                 = QueryIsolationLevel::Serializable;
    d.default_query_options.lock_timeout_ms           = 5000;
    d.default_query_options.max_arena_mib             = 512;
    d.default_query_options.text_format               = TextOutputFormat::CustomDelimited;
    d.default_query_options.custom_delimiter          = '|';
    d.default_query_options.text_include_query        = true;
    d.default_query_options.text_right_align_numeric  = true;
    d.query_history.enabled                           = false;
    d.query_history.retention_days                    = 30;
    d.query_history.max_executions                    = 500;
    d.query_history.max_sql_kib                       = 16;
    REQUIRE(save_environment_options(env_file, d));

    const EnvironmentDocument loaded = load_environment_options(env_file);
    REQUIRE(loaded.default_query_options.execution_timeout_seconds == 45);
    REQUIRE(loaded.default_query_options.nocount);
    REQUIRE(loaded.default_query_options.isolation == QueryIsolationLevel::Serializable);
    REQUIRE(loaded.default_query_options.lock_timeout_ms == 5000);
    REQUIRE(loaded.default_query_options.max_arena_mib == 512u);
    REQUIRE(loaded.default_query_options.text_format == TextOutputFormat::CustomDelimited);
    REQUIRE(loaded.default_query_options.custom_delimiter == '|');
    REQUIRE(loaded.default_query_options.text_include_query);
    REQUIRE(loaded.default_query_options.text_right_align_numeric);
    REQUIRE_FALSE(loaded.query_history.enabled);
    REQUIRE(loaded.query_history.retention_days == 30);
    REQUIRE(loaded.query_history.max_executions == 500);
    REQUIRE(loaded.query_history.max_sql_kib == 16);

    // Server Groups & disable_query_history flag
    const std::filesystem::path prof_file = tmp.path() / "connections.json";
    REQUIRE(add_connection_profile_group(prof_file, "Production/Primary"));
    REQUIRE(add_connection_profile_group(prof_file, "Staging"));
    auto groups = load_connection_profile_groups(prof_file);
    REQUIRE(groups.size() == 2);

    ConnectionProfile p;
    p.id                    = generate_uuid();
    p.name                  = "ProdPG";
    p.engine                = "PostgreSql";
    p.host                  = "prod.local";
    p.port                  = 5432;
    p.database              = "appdb";
    p.username              = "app";
    p.group                 = "Production/Primary";
    p.disable_query_history = true;
    REQUIRE(save_connection_profile(prof_file, p));
    REQUIRE(is_query_history_disabled_for_connection(prof_file, "PostgreSql", "prod.local", 5432));
    REQUIRE_FALSE(is_query_history_disabled_for_connection(prof_file, "PostgreSql", "other.local", 5432));

    REQUIRE(rename_connection_profile_group(prof_file, "Production/Primary", "Prod/Main"));
    const auto reloaded = load_connection_profiles(prof_file);
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0].group == std::optional<std::string>{"Prod/Main"});

    REQUIRE(remove_connection_profile_group(prof_file, "Prod/Main"));
    REQUIRE_FALSE(load_connection_profiles(prof_file)[0].group.has_value());
}

TEST_CASE("QueryHistoryStore encrypts entries, groups by SHA-256 hash, preserves pinned queries on prune, and truncates by KiB limit", "[core][config][history]") {
    using namespace capybaradb::core;

    REQUIRE(normalize_query_history_sql("  SELECT   1\r\n  FROM\tusers  ") == "SELECT 1 FROM users");
    const std::string h1 = hash_query_history_sql("SELECT 1 FROM users");
    const std::string h2 = hash_query_history_sql("  SELECT   1\nFROM\tusers ");
    const std::string h3 = hash_query_history_sql("select 1 from users"); // case-sensitive
    REQUIRE(h1.size() == 64u);
    REQUIRE(h1 == h2);
    REQUIRE(h1 != h3);

    TempDirGuard tmp("history");
    const std::filesystem::path hpath = tmp.path() / "query-history.dat";

    QueryHistorySettings settings;
    settings.enabled        = true;
    settings.retention_days = 7;
    settings.max_executions = 100;
    settings.max_sql_kib    = 1; // 1 KiB limit to verify truncation

    const int64_t day_ms = 24LL * 60 * 60 * 1000;
    const int64_t now_ms = 1750000000000LL;

    // Record an old query (30 days ago) and pin it so retention MUST keep its newest execution
    QueryHistoryCapture pinned_cap;
    pinned_cap.sql            = "SELECT 'secret_pinned_sql' FROM accounts;";
    pinned_cap.engine         = "PostgreSql";
    pinned_cap.server         = "localhost";
    pinned_cap.port           = 5432;
    pinned_cap.database       = "appdb";
    pinned_cap.user           = "postgres";
    pinned_cap.started_utc_ms = now_ms - 30 * day_ms;
    pinned_cap.duration_ms    = 42;
    pinned_cap.rows_returned  = 5;
    REQUIRE(record_query_history_entry(hpath, pinned_cap, settings, pinned_cap.started_utc_ms));

    // Verify encrypted on disk (plaintext SQL must NOT appear)
    const std::string raw_file = read_all_bytes(hpath);
    REQUIRE_FALSE(raw_file.empty());
    REQUIRE(raw_file.find("secret_pinned_sql") == std::string::npos);

    const auto g0 = load_query_history_groups(hpath);
    REQUIRE(g0.size() == 1);
    REQUIRE(toggle_query_history_pin(hpath, g0[0].key));

    // Record an unpinned old query (should be pruned) and a long current query (>1 KiB, truncated)
    QueryHistoryCapture old_cap = pinned_cap;
    old_cap.sql = "SELECT 999;";
    REQUIRE(record_query_history_entry(hpath, old_cap, settings, pinned_cap.started_utc_ms));

    QueryHistoryCapture long_cap = pinned_cap;
    long_cap.sql            = "SELECT '" + std::string(1500, 'A') + "';";
    long_cap.started_utc_ms = now_ms;
    REQUIRE(record_query_history_entry(hpath, long_cap, settings, now_ms));

    // Prune at `now_ms`: unpinned 30-day entry pruned, pinned 30-day entry preserved!
    REQUIRE(prune_query_history(hpath, settings, now_ms));
    const auto groups = load_query_history_groups(hpath);
    REQUIRE(groups.size() == 2);

    bool saw_pinned = false;
    bool saw_trunc  = false;
    for (const auto& g : groups) {
        if (g.pinned) {
            saw_pinned = true;
            REQUIRE(g.latest().sql.find("secret_pinned_sql") != std::string::npos);
        } else {
            saw_trunc = g.latest().truncated();
            REQUIRE(g.latest().sql.size() <= 1024u);
            REQUIRE(g.latest().reopen_text().find("-- Truncated") != std::string::npos);
        }
    }
    REQUIRE(saw_pinned);
    REQUIRE(saw_trunc);
}

TEST_CASE("QueryRecoveryStore persists encrypted drafts and enforces size and path traversal guards", "[core][config][recovery]") {
    using namespace capybaradb::core;

    TempDirGuard tmp("recovery");
    const std::filesystem::path rec_dir = tmp.path() / "recovery";

    RecoveryDraft d;
    d.id                      = generate_uuid();
    d.title                   = "SQLQuery7.sql";
    d.text                    = "SELECT * FROM sensitive_draft_table WHERE id = 42;";
    d.engine                  = "SqlServer";
    d.command_timeout_seconds = 30;
    d.word_wrap               = true;
    d.show_white_space        = true;
    d.updated_utc_ms          = 1750000000000LL;

    REQUIRE(save_recovery_draft(rec_dir, d).has_value());
    const auto ids = list_recovery_draft_ids(rec_dir);
    REQUIRE(ids.size() == 1);
    REQUIRE(ids[0] == d.id);

    // Ensure disk file is encrypted (no plaintext SQL) and zero leftover .partial files
    for (const auto& entry : std::filesystem::directory_iterator(rec_dir)) {
        REQUIRE(entry.path().extension() == ".draft");
        const std::string raw = read_all_bytes(entry.path());
        REQUIRE(raw.find("sensitive_draft_table") == std::string::npos);
    }

    const auto read_back = read_recovery_draft(rec_dir, d.id);
    REQUIRE(read_back.has_value());
    REQUIRE(read_back->title == "SQLQuery7.sql");
    REQUIRE(read_back->text == d.text);
    REQUIRE(read_back->engine == "SqlServer");
    REQUIRE(read_back->command_timeout_seconds == 30);
    REQUIRE(read_back->word_wrap);
    REQUIRE(read_back->show_white_space);

    // Reject oversized draft (> 256 KiB) and invalid ID
    RecoveryDraft huge = d;
    huge.text.assign(RecoveryDraft::kMaxTextBytes + 10, 'x');
    REQUIRE_FALSE(save_recovery_draft(rec_dir, huge).has_value());

    RecoveryDraft bad_id = d;
    bad_id.id = "../escape";
    REQUIRE_FALSE(save_recovery_draft(rec_dir, bad_id).has_value());

    REQUIRE(remove_recovery_draft(rec_dir, d.id));
    REQUIRE(list_recovery_draft_ids(rec_dir).empty());
}

TEST_CASE("ResultExporter streams CSV, JSON, and .rpt files with zero snapshots and cleans up partial files on cancellation", "[core][result][export]") {
    using namespace capybaradb::core;

    QueryExecutionResult qres;
    const std::array<ColumnSpec, 3> cols = {{
        {"id", "int4", LogicalTypeKind::Int, false},
        {"name", "text", LogicalTypeKind::Text, true},
        {"name", "text", LogicalTypeKind::Text, true}, // Intentional duplicate column name for JSON test
    }};
    ResultSetStorage* rs = qres.create_result_set(cols);
    REQUIRE(rs != nullptr);
    {
        ResultSetWriter w(qres, *rs, 64ull * 1024 * 1024, 65536u);
        auto add = [&](const char* a, const char* b, const char* c) {
            REQUIRE(w.begin_row());
            w.write_utf8(0, a, std::strlen(a));
            if (b) w.write_utf8(1, b, std::strlen(b)); else w.write_null(1);
            if (c) w.write_utf8(2, c, std::strlen(c)); else w.write_null(2);
            (void)w.end_row(false);
        };
        add("10", "Alice, \"Jr\"\r\nLine2", "alpha");
        add("2",  nullptr,                  "Ol\xC3\xA1");
        add("30", "Bob;Tab\tEnd",           "gamma");
        w.finish();
    }

    TempDirGuard tmp("export");
    CancelToken cancel;

    // 1. CSV: semicolon, BOM, headers, unquoted \N for NULL, sorted row_order [1,0,2], flatten line breaks
    {
        ExportOptions opts;
        opts.format             = ExportFormat::Csv;
        opts.scope              = ExportScope::AllDisplayedRows;
        opts.csv_delimiter      = ';';
        opts.include_headers    = true;
        opts.utf8_bom           = true;
        opts.retain_line_breaks = false;
        opts.row_order          = {1u, 0u, 2u}; // id: 2, 10, 30

        const auto dest = tmp.path() / "out.csv";
        const auto sum = export_result_set(*rs, dest, opts, cancel);
        REQUIRE(sum.has_value());
        REQUIRE(sum->rows_written == 3u);

        const std::string data = read_all_bytes(dest);
        REQUIRE(data.size() >= 3u);
        REQUIRE(static_cast<unsigned char>(data[0]) == 0xEFu);
        REQUIRE(static_cast<unsigned char>(data[1]) == 0xBBu);
        REQUIRE(static_cast<unsigned char>(data[2]) == 0xBFu);
        REQUIRE(data.find("\"id\";\"name\";\"name\"\r\n") != std::string::npos);
        REQUIRE(data.find("\"2\";\\N;\"Ol\xC3\xA1\"\r\n") != std::string::npos);
        REQUIRE(data.find("\"10\";\"Alice, \"\"Jr\"\" Line2\";\"alpha\"\r\n") != std::string::npos);
    }

    // 2. JSON: SelectedRows (view row 0 and 2 under row_order [1,0,2]), duplicate column names preserved
    {
        ExportOptions opts;
        opts.format          = ExportFormat::Json;
        opts.scope           = ExportScope::SelectedRows;
        opts.row_order       = {1u, 0u, 2u};
        opts.selected_ranges = {CellRange{0, 0, 0, 2}, CellRange{2, 2, 0, 0}};

        const auto dest = tmp.path() / "out.json";
        const auto sum = export_result_set(*rs, dest, opts, cancel);
        REQUIRE(sum.has_value());
        REQUIRE(sum->rows_written == 2u);

        const nlohmann::json j = nlohmann::json::parse(read_all_bytes(dest));
        REQUIRE(j["columns"].size() == 3u);
        REQUIRE(j["columns"][1] == "name");
        REQUIRE(j["columns"][2] == "name");
        REQUIRE(j["rows"].size() == 2u);
        REQUIRE(j["rows"][0][0] == "2");
        REQUIRE(j["rows"][0][1].is_null());
        REQUIRE(j["rows"][1][0] == "30");
    }

    // 3. Canceled export leaves zero destination and zero .partial files
    {
        CancelToken pre_canceled;
        pre_canceled.cancel();
        const auto dest = tmp.path() / "canceled.csv";
        const auto sum = export_result_set(*rs, dest, ExportOptions{}, pre_canceled);
        REQUIRE_FALSE(sum.has_value());
        REQUIRE_FALSE(std::filesystem::exists(dest));
        for (const auto& entry : std::filesystem::directory_iterator(tmp.path())) {
            REQUIRE(entry.path().extension() != ".partial");
        }
    }

    // 4. Results to Text (all formats, right-align numerics, include_query, UTF-8 safe max_characters)
    {
        TextResultsFormat tf;
        tf.output               = TextOutputFormat::ColumnAligned;
        tf.include_headers      = true;
        tf.include_query        = true;
        tf.right_align_numeric  = true;
        tf.max_characters       = 5;
        tf.show_completion_time = true;

        const std::string txt = format_results_as_text(
            qres, 0.250, tf, std::nullopt, "SELECT id, name FROM t;");
        REQUIRE(txt.find("SELECT id, name FROM t;") != std::string::npos);
        REQUIRE(txt.find(" 2 ") != std::string::npos); // right-aligned numeric '2' inside width-2 column
        REQUIRE(txt.find("Alice") != std::string::npos);
        REQUIRE(txt.find("Alice,") == std::string::npos); // truncated cleanly at 5 chars
        REQUIRE(txt.find("Completion time: ") != std::string::npos);

        TextResultsFormat custom_tf = tf;
        custom_tf.output           = TextOutputFormat::CustomDelimited;
        custom_tf.custom_delimiter = '|';
        custom_tf.max_characters   = 256;
        const std::string piped = format_results_as_text(qres, 0.1, custom_tf, std::nullopt, {});
        REQUIRE(piped.find("id|name|name\n") != std::string::npos);
        REQUIRE(piped.find("2|NULL|Ol\xC3\xA1\n") != std::string::npos);
    }

    // 5. Results to File (.rpt): UTF-8 BOM + CRLF line endings
    {
        const auto rpt_path = tmp.path() / "report.rpt";
        TextResultsFormat tf;
        tf.output               = TextOutputFormat::TabDelimited;
        tf.include_query        = true;
        tf.show_completion_time = true;

        const auto sum = write_results_to_rpt_file(
            qres, rpt_path, tf, 0.500, std::nullopt, "SELECT 1;", cancel);
        REQUIRE(sum.has_value());
        REQUIRE(sum->rows_written == 3u);

        const std::string raw = read_all_bytes(rpt_path);
        REQUIRE(raw.size() > 10u);
        REQUIRE(static_cast<unsigned char>(raw[0]) == 0xEFu);
        REQUIRE(static_cast<unsigned char>(raw[1]) == 0xBBu);
        REQUIRE(static_cast<unsigned char>(raw[2]) == 0xBFu);
        REQUIRE(raw.find("SELECT 1;\r\n") != std::string::npos);
        REQUIRE(raw.find("id\tname\tname\r\n") != std::string::npos);
        REQUIRE(raw.find("(3 row(s) returned)\r\n") != std::string::npos);
        // Every LF must be preceded by CR
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\n') {
                REQUIRE((i > 0 && raw[i - 1] == '\r'));
            }
        }
    }
}

TEST_CASE("ExecutionPlanParser parses PostgreSQL JSON and SQL Server ShowPlan XML with warnings, missing indexes, search, and file round-trip", "[core][plan]") {
    using namespace capybaradb::core;

    // 1. PostgreSQL EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) with heavy filter discard warning
    const std::string pg_json = R"json([
      {
        "Plan": {
          "Node Type": "Sort",
          "Total Cost": 250.0,
          "Startup Cost": 220.0,
          "Plan Rows": 100,
          "Actual Rows": 80,
          "Actual Loops": 1,
          "Actual Total Time": 12.5,
          "Sort Key": ["created_at DESC"],
          "Plans": [
            {
              "Node Type": "Seq Scan",
              "Relation Name": "orders",
              "Schema": "public",
              "Alias": "o",
              "Filter": "(status = 'open'::text)",
              "Total Cost": 200.0,
              "Startup Cost": 0.0,
              "Plan Rows": 100,
              "Actual Rows": 80,
              "Actual Loops": 1,
              "Actual Total Time": 10.0,
              "Rows Removed by Filter": 50000
            }
          ]
        },
        "Planning Time": 0.4,
        "Execution Time": 12.8
      }
    ])json";

    RawExecutionPlan pg_raw;
    pg_raw.source_kind = PlanSourceKind::PostgreSqlJson;
    pg_raw.raw_text    = pg_json;
    pg_raw.analyzed    = true;
    pg_raw.query_text  = "SELECT * FROM public.orders o WHERE status = 'open' ORDER BY created_at DESC;";

    const auto pg_parsed = parse_execution_plans({pg_raw}, pg_raw.query_text);
    REQUIRE(pg_parsed.has_value());
    REQUIRE(pg_parsed->size() == 1u);
    const ExecutionPlanStatement& pg_stmt = (*pg_parsed)[0];
    REQUIRE(pg_stmt.root.is_statement);
    REQUIRE(pg_stmt.root.children.size() == 1u);
    const ExecutionPlanOperator& sort_op = pg_stmt.root.children[0];
    REQUIRE(sort_op.title == "Sort");
    REQUIRE(sort_op.children.size() == 1u);
    const ExecutionPlanOperator& scan = sort_op.children[0];
    REQUIRE(scan.title == "Seq Scan");
    REQUIRE(scan.object_name.find("orders") != std::string::npos);
    REQUIRE_FALSE(scan.warnings.empty());
    REQUIRE(scan.cost_percent > 70.0);
    REQUIRE_FALSE(pg_stmt.missing_indexes.empty());
    REQUIRE(pg_stmt.missing_indexes[0].commented_script.find("/*") != std::string::npos); // Commented, never auto-executed

    // 2. SQL Server ShowPlan XML with 2 statements, Actual counters, Warnings, and MissingIndexes
    const std::string mssql_xml = R"xml(<?xml version="1.0" encoding="utf-16"?>
    <ShowPlanXML xmlns="http://schemas.microsoft.com/sqlserver/2004/07/showplan">
      <BatchSequence><Batch><Statements>
        <StmtSimple StatementId="1" StatementText="SELECT * FROM dbo.Orders WHERE CustomerId = 5" StatementSubTreeCost="0.80">
          <QueryPlan>
            <MissingIndexes>
              <MissingIndexGroup Impact="94.5">
                <MissingIndex Database="[SalesDb]" Schema="[dbo]" Table="[Orders]">
                  <ColumnGroup Usage="EQUALITY"><Column Name="[CustomerId]"/></ColumnGroup>
                  <ColumnGroup Usage="INCLUDE"><Column Name="[Total]"/></ColumnGroup>
                </MissingIndex>
              </MissingIndexGroup>
            </MissingIndexes>
            <RelOp NodeId="0" PhysicalOp="Clustered Index Scan" LogicalOp="Clustered Index Scan" EstimateRows="250" EstimatedTotalSubtreeCost="0.80" EstimateCPU="0.10" EstimateIO="0.70">
              <Warnings NoJoinPredicate="true"/>
              <RunTimeInformation>
                <RunTimeCountersPerThread Thread="0" ActualRows="12" ActualExecutions="1" ActualElapsedms="15"/>
              </RunTimeInformation>
              <IndexScan>
                <Object Database="[SalesDb]" Schema="[dbo]" Table="[Orders]" Index="[PK_Orders]"/>
              </IndexScan>
            </RelOp>
          </QueryPlan>
        </StmtSimple>
        <StmtSimple StatementId="2" StatementText="SELECT 1" StatementSubTreeCost="0.20">
          <QueryPlan>
            <RelOp NodeId="0" PhysicalOp="Constant Scan" LogicalOp="Constant Scan" EstimateRows="1" EstimatedTotalSubtreeCost="0.20"/>
          </QueryPlan>
        </StmtSimple>
      </Statements></Batch></BatchSequence>
    </ShowPlanXML>)xml";

    RawExecutionPlan ms_raw;
    ms_raw.source_kind = PlanSourceKind::SqlServerXml;
    ms_raw.raw_text    = mssql_xml;
    ms_raw.analyzed    = true;

    const auto ms_parsed = parse_execution_plans({ms_raw});
    REQUIRE(ms_parsed.has_value());
    REQUIRE(ms_parsed->size() == 2u);
    REQUIRE((*ms_parsed)[0].batch_percent > 79.0);
    REQUIRE((*ms_parsed)[1].batch_percent < 21.0);
    REQUIRE((*ms_parsed)[0].root.children.size() == 1u);
    const ExecutionPlanOperator& ms_scan = (*ms_parsed)[0].root.children[0];
    REQUIRE(ms_scan.title == "Clustered Index Scan");
    REQUIRE_FALSE(ms_scan.warnings.empty());
    REQUIRE(ms_scan.actual_rows == std::optional<double>{12.0});
    REQUIRE((*ms_parsed)[0].missing_indexes.size() == 1u);
    REQUIRE((*ms_parsed)[0].missing_indexes[0].commented_script.find("CREATE NONCLUSTERED INDEX") != std::string::npos);

    // 3. ExecutionPlanSearch (Contains / wrap-around) + save/load round-trip
    const auto ops = collect_plan_operators(*ms_parsed);
    REQUIRE(ops.size() == 4u); // 2 statement roots + 2 RelOps
    const auto* hit = find_next_plan_operator(ops, nullptr, "PhysicalOp", ExecutionPlanComparison::Contains, "Constant");
    REQUIRE(hit != nullptr);
    REQUIRE(hit->title == "Constant Scan");

    TempDirGuard tmp("plan");
    const auto sqlplan_path = tmp.path() / "saved.sqlplan";
    REQUIRE(save_raw_execution_plan(ms_raw, sqlplan_path).has_value());
    const auto reloaded = load_execution_plan_file(sqlplan_path);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->size() == 2u);
    REQUIRE((*reloaded)[0].root.children[0].title == "Clustered Index Scan");
}

TEST_CASE("SqlCompletionEngine resolves accent-insensitive table, alias, foreign-key JOIN, and bracket-quoted completions", "[core][completion]") {
    using namespace capybaradb::core;

    REQUIRE(fold_accents_lower("Usu\xC3\xA1rios") == "usuarios");
    REQUIRE(fold_accents_lower("A\xC3\xA7\xC3\xA3o_M\xC3\xAA" "s") == "acao_mes");
    REQUIRE(accent_starts_with("Usu\xC3\xA1rios", "usuar"));
    REQUIRE(accent_contains("C\xC3\xB3" "digoCliente", "codigo"));

    // PostgreSQL catalog with accented table & FK
    SqlCompletionCatalog pg_cat;
    pg_cat.is_tsql     = false;
    pg_cat.database    = "appdb";
    pg_cat.search_path = {"public"};
    pg_cat.schemas     = {"public", "vendas"};
    pg_cat.relations   = {
        {"public", "Usu\xC3\xA1rios", false},
        {"public", "orders", false},
        {"vendas", "faturas", true},
    };
    pg_cat.set_relation_columns("public", "Usu\xC3\xA1rios", {
        {"id", "int4", false},
        {"c\xC3\xB3" "digo", "text", true},
    });
    pg_cat.set_relation_columns("public", "orders", {
        {"id", "int4", false},
        {"user_id", "int4", false},
        {"total", "numeric", false},
    });
    REQUIRE(pg_cat.all_columns.size() == 5u);
    REQUIRE(pg_cat.relation_columns(0).size() == 2u);
    REQUIRE(pg_cat.relation_columns(1).size() == 3u);

    // Out-of-order append_column maintains contiguous slices in all_columns for each relation
    pg_cat.append_column("vendas", "faturas", {"id_fatura", "int8", false});
    pg_cat.append_column("public", "Usu\xC3\xA1rios", {"email", "text", true});
    pg_cat.append_column("vendas", "faturas", {"valor", "numeric", false});
    REQUIRE(pg_cat.all_columns.size() == 8u);
    REQUIRE(pg_cat.relation_columns(0).size() == 3u);
    REQUIRE(pg_cat.relation_columns(0)[2].name == "email");
    REQUIRE(pg_cat.relation_columns(1).size() == 3u);
    REQUIRE(pg_cat.relation_columns(1)[2].name == "total");
    REQUIRE(pg_cat.relation_columns(2).size() == 2u);
    REQUIRE(pg_cat.relation_columns(2)[0].name == "id_fatura");
    REQUIRE(pg_cat.relation_columns(2)[1].name == "valor");
    pg_cat.foreign_keys.push_back(SqlCatalogForeignKey{
        .name           = "fk_orders_user",
        .schema         = "public",
        .table          = "orders",
        .columns        = {"user_id"},
        .target_schema  = "public",
        .target_table   = "Usu\xC3\xA1rios",
        .target_columns = {"id"},
    });

    // 1. FROM accent-insensitive completion: "SELECT * FROM usuar" -> matches "Usuários"
    {
        const std::string sql = "SELECT * FROM usuar";
        const auto res = complete_sql(sql, sql.size(), pg_cat);
        REQUIRE(res.has_value());
        const auto filtered = filter_sql_completion_items(res->items, res->prefix);
        REQUIRE_FALSE(filtered.empty());
        REQUIRE(filtered[0].text.find("Usu\xC3\xA1rios") != std::string::npos);
    }

    // 2. alias. column completion: "SELECT o. FROM orders o"
    {
        const std::string sql = "SELECT o. FROM orders o";
        const size_t caret = 9; // right after "o."
        const auto res = complete_sql(sql, caret, pg_cat);
        REQUIRE(res.has_value());
        bool found_total = false;
        for (const auto& it : res->items) {
            if (it.text == "total") found_total = true;
        }
        REQUIRE(found_total);
    }

    // 3. JOIN FK suggestion
    {
        const std::string sql = "SELECT * FROM orders o JOIN ";
        const auto res = complete_sql(sql, sql.size(), pg_cat);
        REQUIRE(res.has_value());
        bool has_fk_join = false;
        for (const auto& it : res->items) {
            if (it.kind == SqlCompletionKind::Join && it.text.find("ON ") != std::string::npos) {
                has_fk_join = true;
            }
        }
        REQUIRE(has_fk_join);
    }

    // 4. Inside string or comment returns nullopt
    {
        const std::string in_str = "SELECT 'FROM usuar";
        REQUIRE_FALSE(complete_sql(in_str, in_str.size(), pg_cat).has_value());
        const std::string in_cmt = "SELECT 1 -- FROM usuar";
        REQUIRE_FALSE(complete_sql(in_cmt, in_cmt.size(), pg_cat).has_value());
    }

    // 5. SQL Server [bracket] identifier quoting & schema. completion
    SqlCompletionCatalog ms_cat;
    ms_cat.is_tsql     = true;
    ms_cat.database    = "SalesDb";
    ms_cat.search_path = {"dbo"};
    ms_cat.schemas     = {"dbo"};
    ms_cat.relations   = {{"dbo", "Order Details", false}};
    ms_cat.set_relation_columns("dbo", "Order Details", {{"Unit Price", "money", false}});

    {
        const std::string sql = "SELECT * FROM dbo.";
        const auto res = complete_sql(sql, sql.size(), ms_cat);
        REQUIRE(res.has_value());
        bool found_bracketed = false;
        for (const auto& it : res->items) {
            if (it.text == "[Order Details]") found_bracketed = true;
        }
        REQUIRE(found_bracketed);
    }
}

TEST_CASE("PostgreSQL OID classifier and SQL Server type utilities handle arrays, ranges, citext, WCHAR bindings, and hierarchyid decoding", "[drivers][types]") {
    using namespace capybaradb::drivers;
    using capybaradb::core::LogicalTypeKind;

    // PostgreSQL built-in OIDs & extension types
    REQUIRE(lookup_pg_oid(1009u).kind == PgLogicalKind::Array);   // text[]
    REQUIRE(lookup_pg_oid(3807u).kind == PgLogicalKind::Array);   // jsonb[]
    REQUIRE(lookup_pg_oid(3904u).kind == PgLogicalKind::Text);    // int4range
    REQUIRE(lookup_pg_oid(4535u).kind == PgLogicalKind::Text);    // tstzmultirange
    REQUIRE(lookup_pg_oid(600u).kind == PgLogicalKind::Text);     // point
    REQUIRE(lookup_pg_oid(718u).kind == PgLogicalKind::Text);     // circle
    REQUIRE(lookup_pg_oid(17u).kind == PgLogicalKind::Binary);    // bytea
    REQUIRE(lookup_pg_oid(790u).kind == PgLogicalKind::Numeric);  // money
    REQUIRE(lookup_pg_oid(1186u).kind == PgLogicalKind::Text);    // interval

    bool json_ok = false;
    REQUIRE(classify_pg_type_name("citext", json_ok) == LogicalTypeKind::Text);
    REQUIRE(json_ok);
    REQUIRE(classify_pg_type_name("_citext", json_ok) == LogicalTypeKind::Array);
    REQUIRE(classify_pg_type_name("int4range", json_ok) == LogicalTypeKind::Text);

    // SQL Server ODBC vendor types + collation-safe SQL_C_WCHAR binding rule
    REQUIRE(should_bind_mssql_as_wchar(kOdbcTypeChar));
    REQUIRE(should_bind_mssql_as_wchar(kOdbcTypeVarchar));
    REQUIRE(should_bind_mssql_as_wchar(kOdbcTypeWVarchar));
    REQUIRE(should_bind_mssql_as_wchar(kSqlSsXml));
    REQUIRE(should_bind_mssql_as_wchar(kSqlSsVariant));
    REQUIRE_FALSE(should_bind_mssql_as_wchar(kOdbcTypeInteger));
    REQUIRE(is_mssql_binary_type(kSqlSsUdt));

    REQUIRE(classify_mssql_column(kSqlSsTimestampOffset, "datetimeoffset", json_ok) == LogicalTypeKind::Timestamp);
    REQUIRE(classify_mssql_column(kSqlSsTime2, "time", json_ok) == LogicalTypeKind::Time);
    REQUIRE(classify_mssql_column(kSqlSsXml, "xml", json_ok) == LogicalTypeKind::Xml);
    REQUIRE(classify_mssql_column(kSqlSsVariant, "sql_variant", json_ok) == LogicalTypeKind::Unknown);
    REQUIRE(json_ok);
    REQUIRE(classify_mssql_column(kOdbcTypeDecimal, "money", json_ok) == LogicalTypeKind::Numeric);
    REQUIRE(classify_mssql_column(kOdbcTypeGuid, "uniqueidentifier", json_ok) == LogicalTypeKind::Uuid);
    REQUIRE(classify_mssql_column(kSqlSsUdt, "hierarchyid", json_ok) == LogicalTypeKind::Text);

    // SQL Server hierarchyid Known-Prefix OrdPath binary decoder
    REQUIRE(decode_mssql_hierarchyid({}) == std::optional<std::string>{"/"});
    {
        const std::array<uint8_t, 1> b1 = {0x58}; // /1/
        REQUIRE(decode_mssql_hierarchyid(b1) == std::optional<std::string>{"/1/"});
    }
    {
        const std::array<uint8_t, 2> b2 = {0x5B, 0x40}; // /1/2/
        REQUIRE(decode_mssql_hierarchyid(b2) == std::optional<std::string>{"/1/2/"});
    }
    {
        const std::array<uint8_t, 2> b3 = {0x54, 0x60}; // /1.5/
        REQUIRE(decode_mssql_hierarchyid(b3) == std::optional<std::string>{"/1.5/"});
    }
    {
        const std::array<uint8_t, 1> b4 = {0x3F}; // /-1/
        REQUIRE(decode_mssql_hierarchyid(b4) == std::optional<std::string>{"/-1/"});
    }
    {
        const std::array<uint8_t, 1> bad = {0x50}; // /1. (unterminated dotted ordinal)
        REQUIRE_FALSE(decode_mssql_hierarchyid(bad).has_value());
    }
}
